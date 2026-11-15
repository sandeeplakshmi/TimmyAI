#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
#
# License checks for the Timmy repository (Apache-2.0):
#   - LICENSE and NOTICE exist, and NOTICE keeps the upstream attribution
#   - every source file has an SPDX header or the full Apache-2.0 header
#   - upstream "Copyright (c) Meta Platforms, Inc. and affiliates." lines are unchanged
#   - an upstream file without "Modified by the Timmy contributors" is byte-for-byte the upstream
#     file (scripts/upstream-manifest.txt holds their SHA-256s); changed ones must carry the line
#   - bundled third-party code keeps its license text
# Exit: 0 clean, 1 findings.
set -uo pipefail
root=$(git rev-parse --show-toplevel 2>/dev/null) || root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
fail=0
err() { echo "LICENSE CHECK: $*"; fail=1; }

[ -f LICENSE ] && grep -q "Apache License" LICENSE && grep -q "Version 2.0, January 2004" LICENSE || err "LICENSE missing or not Apache-2.0"
[ -f NOTICE ] || err "NOTICE missing"
grep -q "Based on the Muse Gadgets SDK by Meta Platforms, Inc. and affiliates" NOTICE 2>/dev/null || err "NOTICE lost the upstream attribution"

if git rev-parse --git-dir >/dev/null 2>&1; then files=$(git ls-files); else files=$(find . -type f -not -path './.git/*' | sed 's#^\./##'); fi

third_party() { case "$1" in web/vendor/*|firmware/components/minimp3/*) return 0;; esac; return 1; }
is_source() {
    case "$1" in
        *.c|*.h|*.cpp|*.hpp|*.S|*.py|*.sh|*.js|*.mjs|*.html|*.cmake|*.lf|*/CMakeLists.txt|CMakeLists.txt|*/Kconfig|*/Kconfig.projbuild) return 0;;
    esac
    return 1
}
MANIFEST=scripts/upstream-manifest.txt
META_LINE_RE='^[[:space:]]*(#|//|/?\*|<!--)?[[:space:]]*Copyright \(c\) Meta Platforms, Inc\. and affiliates\.[[:space:]]*(\*/)?$'

while IFS= read -r f; do
    [ -f "$f" ] || continue
    third_party "$f" && continue
    case "$f" in NOTICE|README.md|REPORT.md|scripts/check-licenses.sh|scripts/upstream-manifest.txt) attribution=1;; *) attribution=0;; esac
    if [ $attribution = 0 ] && grep -q "Meta Platforms" "$f" 2>/dev/null; then
        bad=$(grep "Meta Platforms" "$f" | grep -vE "$META_LINE_RE" | head -1)
        [ -n "$bad" ] && err "$f: upstream copyright line changed: $bad"
        if ! grep -q "Modified by the Timmy contributors" "$f"; then
            sum=$(shasum -a 256 "$f" | cut -d' ' -f1)
            grep -q "^$sum  $f$" "$MANIFEST" 2>/dev/null || err "$f differs from the upstream file but lacks 'Modified by the Timmy contributors, 2026'"
        fi
    fi
    if is_source "$f"; then
        top=$(sed -n 1,40p "$f")
        printf '%s' "$top" | grep -qE "SPDX-License-Identifier: Apache-2.0|Licensed under the Apache License, Version 2.0" \
            || err "$f has no license header (add: SPDX-License-Identifier: Apache-2.0)"
    fi
done <<< "$files"

# Bundled third-party code keeps its license text.
for lic in firmware/components/minimp3/LICENSE web/vendor/three/LICENSE web/vendor/talkinghead/LICENSE; do
    [ -f "$lic" ] || err "missing third-party license text: $lic"
    grep -q "${lic%/LICENSE}" NOTICE || err "NOTICE doesn't list ${lic%/LICENSE}"
done

[ $fail = 0 ] && echo "check-licenses: clean" || echo "check-licenses: FAILED"
exit $fail
