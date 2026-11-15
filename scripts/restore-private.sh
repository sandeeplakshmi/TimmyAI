#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
#
# Restore your private Timmy folder on a new Mac (see docs/RESTORE.md).
#   scripts/restore-private.sh BACKUP.zip     from an encrypted zip made by backup-private.sh zip
#   scripts/restore-private.sh GIT-URL|PATH   from your private git backup (then copy keys/ and .env back yourself)
# Never overwrites an existing $TIMMY_HOME: move it aside first.
set -euo pipefail
TIMMY_HOME=${TIMMY_HOME:-$HOME/.timmy}
src=${1:?usage: $0 BACKUP.zip | GIT-URL}
[ -e "$TIMMY_HOME" ] && { echo "$TIMMY_HOME already exists; move it aside first (nothing changed)" >&2; exit 1; }
case $src in
    *.zip)
        z=$(command -v 7zz || command -v 7z || true)
        [ -n "$z" ] || { echo "needs 7-Zip: brew install sevenzip" >&2; exit 3; }
        tmp=$(mktemp -d)
        "$z" x -o"$tmp" "$src" >/dev/null
        mv "$tmp"/* "$TIMMY_HOME" && rmdir "$tmp"
        ;;
    *) git clone -q "$src" "$TIMMY_HOME" ;;
esac
chmod 700 "$TIMMY_HOME"; [ -d "$TIMMY_HOME/keys" ] && chmod -R go-rwx "$TIMMY_HOME/keys"
[ -f "$TIMMY_HOME/.env" ] && chmod 600 "$TIMMY_HOME/.env"
echo "✓ restored $TIMMY_HOME. Next: ./install.sh --yes (it keeps this folder as it is), then ./install.sh doctor."
