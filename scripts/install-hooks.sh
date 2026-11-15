#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
# Installs Timmy's pre-commit hook into this clone (and the pre-commit framework's, if it's installed).
set -euo pipefail
root=$(git rev-parse --show-toplevel)
cd "$root"
# A thin wrapper, so updates to scripts/pre-commit apply without reinstalling.
printf '%s\n' '#!/bin/bash' 'exec "$(git rev-parse --show-toplevel)/scripts/pre-commit" "$@"' > .git/hooks/pre-commit
chmod 755 .git/hooks/pre-commit
echo "✓ installed .git/hooks/pre-commit (runs scripts/pre-commit)"
if command -v pre-commit >/dev/null 2>&1; then
    echo "note: the pre-commit framework is installed; .pre-commit-config.yaml runs the same checks (pre-commit run --all-files)."
fi
command -v gitleaks >/dev/null 2>&1 || echo "note: install gitleaks for the secret scan: brew install gitleaks"
[ -f "${TIMMY_HOME:-$HOME/.timmy}/denylist.txt" ] || echo "note: add your private strings to ${TIMMY_HOME:-$HOME/.timmy}/denylist.txt (one per line; it stays outside the repo)"
