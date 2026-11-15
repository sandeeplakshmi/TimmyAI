#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
#
# Back up your private Timmy folder ($TIMMY_HOME, default ~/.timmy) so you can restore it on a new Mac.
#   scripts/backup-private.sh zip [OUT.zip]   an AES-256 encrypted zip (asks for a password; needs 7-Zip:
#                                               brew install sevenzip). Default OUT: ~/timmy-private-<date>.zip
#   scripts/backup-private.sh git             commit $TIMMY_HOME to its own local git repo (keys/ and .env
#                                               excluded); push it only to a private remote you control
# Restore with scripts/restore-private.sh. Never put the backup inside the public repository.
set -euo pipefail
TIMMY_HOME=${TIMMY_HOME:-$HOME/.timmy}
[ -d "$TIMMY_HOME" ] || { echo "no $TIMMY_HOME to back up" >&2; exit 1; }
case ${1:-zip} in
    zip)
        out=${2:-$HOME/timmy-private-$(date +%Y-%m-%d).zip}
        z=$(command -v 7zz || command -v 7z || true)
        [ -n "$z" ] || { echo "needs 7-Zip for an encrypted zip: brew install sevenzip" >&2; exit 3; }
        (cd "$(dirname "$TIMMY_HOME")" && "$z" a -tzip -mem=AES256 -p "$out" "$(basename "$TIMMY_HOME")" -xr'!__pycache__' >/dev/null)
        chmod 600 "$out"
        echo "✓ encrypted backup: $out ($(du -h "$out" | cut -f1)). Keep the password somewhere safe."
        ;;
    git)
        cd "$TIMMY_HOME"
        [ -d .git ] || git init -q -b main
        if [ ! -f .gitignore ]; then
            printf '%s\n' '# Secrets stay out of this repo too: back them up separately (encrypted zip).' \
                '.env' '.env.*' 'keys/' '*.pem' '*.key' '__pycache__/' '*.log' > .gitignore
        fi
        git add -A
        git -c user.name="${GIT_AUTHOR_NAME:-timmy-backup}" -c user.email="${GIT_AUTHOR_EMAIL:-timmy-backup@localhost}" \
            commit -q -m "Backup $(date +%Y-%m-%dT%H:%M)" && echo "✓ committed in $TIMMY_HOME (local only; keys/ and .env excluded)" \
            || echo "nothing new to back up"
        ;;
    *) sed -n 4,11p "$0"; exit 2 ;;
esac
