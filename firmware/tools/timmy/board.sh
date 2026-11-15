#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
#
# Build or flash the Timmy firmware (Waveshare ESP32-S3-Touch-AMOLED-1.75C):
#   tools/timmy/board.sh build
#   tools/timmy/board.sh flash [serial|port]
# Needs ESP-IDF v6.0.3 (set IDF_EXPORT to its export.sh if idf.py isn't on PATH).
set -euo pipefail
cmd=${1:?build|flash}
root=$(cd "$(dirname "$0")/../.." && pwd)
if [ -n "${IDF_EXPORT:-}" ]; then
    . "$IDF_EXPORT" >/dev/null 2>&1
elif ! command -v idf.py >/dev/null 2>&1; then
    for d in "${IDF_PATH:-}" "$HOME/esp/esp-idf-v6.0.3" "$HOME/.espressif/esp-idf-v6.0.3" "$HOME/esp/esp-idf-v6"; do
        [ -n "$d" ] && [ -f "$d/export.sh" ] && { . "$d/export.sh" >/dev/null 2>&1; break; }
    done
fi
command -v idf.py >/dev/null 2>&1 || { echo "idf.py not found; activate ESP-IDF v6.0.3 first" >&2; exit 1; }
cd "$root"
case $cmd in
    build) idf.py build ;;
    flash)
        port=$(python3 "$root/tools/timmy/ports.py" ${2:-})
        idf.py -p "$port" flash ;;
    *) echo "usage: $0 build|flash [serial|port]" >&2; exit 2 ;;
esac
