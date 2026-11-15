#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
#
# Blocks personal data and third-party branding from the repository.
#   scripts/check-private.sh            check every tracked file (or every file, outside git)
#   scripts/check-private.sh --staged   check only what is staged for commit (the pre-commit hook)
# Fails on:
#   - a secret gitleaks finds (when gitleaks is installed; CI always has it)
#   - any string from the private denylist, $TIMMY_HOME/denylist.txt (default ~/.timmy/denylist.txt;
#     one string per line, # comments). That file lives outside the repo and is never printed.
#   - upstream-branding and private-network words outside the allowed attribution places
#   - absolute home-directory paths (/Users/<name>/..., /home/<name>/...)
# Exit: 0 clean, 1 findings, 2 usage error.
set -uo pipefail
root=$(git rev-parse --show-toplevel 2>/dev/null) || root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
MODE=${1:-all}
TIMMY_HOME=${TIMMY_HOME:-$HOME/.timmy}
DENY="$TIMMY_HOME/denylist.txt"
fail=0

if [ "$MODE" = --staged ]; then
    files=$(git diff --cached --name-only --diff-filter=ACMR)
    content() { git show ":$1" 2>/dev/null; }
elif [ "$MODE" = all ]; then
    if git rev-parse --git-dir >/dev/null 2>&1; then files=$(git ls-files); else files=$(find . -type f -not -path './.git/*' | sed 's#^\./##'); fi
    content() { cat "$1" 2>/dev/null; }
else
    echo "usage: $0 [--staged]" >&2; exit 2
fi

# Files allowed to name the upstream project (attribution and provenance only).
allowed_brand() { case "$1" in NOTICE|README.md|REPORT.md|scripts/check-private.sh|scripts/check-licenses.sh|scripts/upstream-manifest.txt) return 0;; esac; return 1; }
skip_binary() { case "$1" in *.png|*.jpg|*.jpeg|*.gif|*.ico|*.glb|*.bin|*.wav|*.mp3|*.ttf|*.woff|*.woff2) return 0;; esac; return 1; }
# Words built from pieces so this script doesn't match itself.
W1="mu""se"; W2="cloud""flared"; W3="trycloud""flare"; W4="tail""scale"; W5="ts""\\.net"
BRAND_RE="($W1|$W2|$W3|$W4|$W5)"

while IFS= read -r f; do
    [ -n "$f" ] || continue
    skip_binary "$f" && continue
    body=$(content "$f") || continue
    if ! allowed_brand "$f"; then
        hits=$(printf '%s' "$body" | grep -niE "$BRAND_RE" | head -3)
        [ -n "$hits" ] && { echo "BRANDING/PRIVATE-NETWORK word in $f:"; echo "$hits" | cut -c1-160; fail=1; }
        if printf '%s' "$f" | grep -qiE "$BRAND_RE"; then echo "BRANDING word in path: $f"; fail=1; fi
    fi
    hits=$(printf '%s' "$body" | grep -nE "(/Users/[A-Za-z0-9._-]+/|/home/[A-Za-z0-9._-]+/)" | grep -vE "/Users/(you|me|name|USER|<[^>]*>)/|/home/(you|me|name|user|runner|<[^>]*>)/" | head -3)
    [ -n "$hits" ] && { echo "ABSOLUTE HOME PATH in $f:"; echo "$hits" | cut -c1-160; fail=1; }
    if [ -f "$DENY" ]; then
        while IFS= read -r word; do
            word=$(printf '%s' "$word" | sed 's/[[:space:]]*$//')
            case "$word" in ''|'#'*) continue;; esac
            if printf '%s' "$body" | grep -qiF -- "$word" || printf '%s' "$f" | grep -qiF -- "$word"; then
                echo "DENYLIST string found in $f (line $(printf '%s' "$body" | grep -niF -- "$word" | head -1 | cut -d: -f1); string not shown)"
                fail=1
            fi
        done < "$DENY"
    fi
done <<< "$files"

if command -v gitleaks >/dev/null 2>&1; then
    if [ "$MODE" = --staged ]; then
        gitleaks git --staged --no-banner --no-color --redact -v . >/tmp/timmy-gitleaks.$$ 2>&1 || { echo "GITLEAKS found a secret in the staged changes:"; grep -E "Finding|File|RuleID|Line" /tmp/timmy-gitleaks.$$ | head -20; fail=1; }
    else
        # Only the repository's own files (not build output or fetched components).
        tmp=$(mktemp -d)
        printf '%s\n' "$files" | rsync -a --files-from=- ./ "$tmp/" 2>/dev/null
        (cd "$tmp" && gitleaks dir --no-banner --no-color --redact -v . >/tmp/timmy-gitleaks.$$ 2>&1) || { echo "GITLEAKS found a secret:"; sed "s#$tmp/##" /tmp/timmy-gitleaks.$$ | grep -E "Finding|File|RuleID|Line" | head -20; fail=1; }
        rm -rf "$tmp"
    fi
    rm -f /tmp/timmy-gitleaks.$$
else
    echo "note: gitleaks not installed (brew install gitleaks); secret scan skipped" >&2
fi

[ $fail = 0 ] && echo "check-private: clean" || echo "check-private: FAILED (fix the files above, or move private things to ~/.timmy)"
exit $fail
