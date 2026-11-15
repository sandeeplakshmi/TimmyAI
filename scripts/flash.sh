#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
#
# Flash Timmy's firmware to the Waveshare ESP32-S3-Touch-AMOLED-1.75C, with guided board steps.
#
#   scripts/flash.sh [options]
#     --port PORT       use this serial port (default: find the board on USB)
#     --image DIR       flash a prebuilt image: a folder with flash_args and its .bin files
#                       (default: build firmware/ first; with private add-ons when $TIMMY_HOME/firmware-extras exists)
#     --public          build without private add-ons
#     --backup FILE     read the board's current firmware (first 8.5 MB) into FILE before flashing
#     --no-wait         don't wait for the board to reach the server afterwards
#     --wait-only       skip flashing; only wait for the board to reach the server
#     --yes             don't ask before flashing (the human already said yes)
#     --json            print one JSON object per line: {"state": ..., "message": ...}
#     --dry-run         show what would happen; change nothing
#
# States (--json): waiting_for_board, board_found, building, backing_up, flashing, flashed,
#                  waiting_for_wifi, connected, error
# Exit codes: 0 ok, 2 usage, 20 BOARD_NOT_FOUND, 21 MULTIPLE_BOARDS, 22 PORT_BUSY, 23 BUILD_FAILED,
#             24 FLASH_FAILED, 25 NOT_CONNECTED (flashed, but the board didn't reach the server),
#             26 DECLINED, 27 NO_ESP_IDF, 28 BACKUP_FAILED
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TIMMY_HOME=${TIMMY_HOME:-$HOME/.timmy}
PORT="" IMAGE="" PUBLIC=0 BACKUP="" WAIT=1 FLASH=1 YES=0 JSON=0 DRY=0
WAIT_BOARD_S=${TIMMY_WAIT_BOARD_S:-180}
WAIT_CONNECT_S=${TIMMY_WAIT_CONNECT_S:-300}
while [ $# -gt 0 ]; do
    case $1 in
        --port) PORT=${2:?}; shift ;;
        --image) IMAGE=${2:?}; shift ;;
        --public) PUBLIC=1 ;;
        --backup) BACKUP=${2:?}; shift ;;
        --no-wait) WAIT=0 ;;
        --wait-only) FLASH=0 ;;
        --yes|-y) YES=1 ;;
        --json) JSON=1 ;;
        --dry-run) DRY=1 ;;
        -h|--help) sed -n 4,26p "$0"; exit 0 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done

emit() {   # emit STATE MESSAGE
    if [ $JSON = 1 ]; then
        python3 -c 'import json,sys; print(json.dumps({"state": sys.argv[1], "message": sys.argv[2]}), flush=True)' "$1" "$2"
    else
        printf '%s\n' "$2"
    fi
}
die() { emit error "$2"; exit "$1"; }

find_ports() {   # the ESP32-S3's own USB Serial/JTAG (VID 0x303A); on macOS /dev/cu.usbmodem*
    [ -n "${TIMMY_SIMULATE_NO_BOARD:-}" ] && return 0   # tests: behave as if nothing is plugged in
    local py
    for py in "${IDF_PYTHON_ENV_PATH:-}/bin/python" python3; do
        if "$py" -c "import serial.tools.list_ports" 2>/dev/null; then
            "$py" - <<'PY'
from serial.tools import list_ports
for p in list_ports.comports():
    if p.vid == 0x303A:
        print(p.device.replace("/dev/tty.", "/dev/cu."))
PY
            return
        fi
    done
    ls /dev/cu.usbmodem* 2>/dev/null
}

wait_for_board() {
    local start=$SECONDS hinted=0 spin='|/-\' i=0 ports
    emit waiting_for_board "Plug the Waveshare board into this Mac with a USB-C data cable... waiting (Ctrl+C to stop)"
    while :; do
        ports=$(find_ports | sort -u)
        if [ -n "$ports" ]; then
            [ $JSON = 0 ] && [ -t 1 ] && printf '\r                                   \r'
            FOUND_PORTS=$ports
            return 0
        fi
        local waited=$((SECONDS - start))
        if [ $hinted = 0 ] && [ $waited -ge 30 ]; then
            hinted=1
            [ $JSON = 0 ] && [ -t 1 ] && printf '\r'
            emit waiting_for_board "Still no board. Try: a different USB-C cable (charge-only cables don't work), another USB port, no hub or dock."
            emit waiting_for_board "If it still doesn't show up, put it in download mode: hold BOOT (the bottom button), tap the power/reset button, then let go of BOOT."
            emit waiting_for_board "Also close any other serial monitor (Arduino, screen, idf.py monitor) that may hold the port."
        fi
        if [ $waited -ge "$WAIT_BOARD_S" ]; then
            [ $JSON = 0 ] && [ -t 1 ] && printf '\n'
            return 1
        fi
        if [ $JSON = 0 ] && [ -t 1 ]; then printf '\r%s waiting for the board (%ss)' "${spin:i++%4:1}" "$waited"; fi
        sleep 1
    done
}

export_idf() {
    command -v idf.py >/dev/null 2>&1 && return 0
    local d
    for d in "${IDF_PATH:-}" "$HOME/esp/esp-idf-v6.0.3" "$HOME/.espressif/esp-idf-v6.0.3" "$HOME/esp/esp-idf-v6" "$HOME/esp/esp-idf"; do
        [ -n "$d" ] && [ -f "$d/export.sh" ] && { . "$d/export.sh" >/dev/null 2>&1; break; }
    done
    command -v idf.py >/dev/null 2>&1
}

if [ $FLASH = 1 ]; then
    # 1. Find the board.
    if [ -z "$PORT" ]; then
        if [ $DRY = 1 ]; then
            ports=$(find_ports | sort -u)
            emit waiting_for_board "dry run: boards on USB now: ${ports:-none}"
            PORT=${ports%%$'\n'*}
        else
            FOUND_PORTS=""
            wait_for_board || die 20 "BOARD_NOT_FOUND: no ESP32-S3 board on USB after ${WAIT_BOARD_S}s. Plug it in (data cable, no hub) or use download mode, then run this again."
            ports=$FOUND_PORTS
            n=$(printf '%s\n' "$ports" | grep -c .)
            if [ "$n" -gt 1 ]; then
                if [ -t 0 ] && [ $JSON = 0 ]; then
                    echo "Several boards found:"; printf '%s\n' "$ports" | nl
                    read -r -p "Which one (number)? " k
                    PORT=$(printf '%s\n' "$ports" | sed -n "${k}p")
                    [ -n "$PORT" ] || die 21 "MULTIPLE_BOARDS: no valid choice"
                else
                    die 21 "MULTIPLE_BOARDS: $(printf '%s' "$ports" | tr '\n' ' ')- pass --port"
                fi
            else
                PORT=$ports
            fi
        fi
    fi
    emit board_found "Board found on ${PORT:-<none>}"
    if [ -n "$PORT" ] && command -v lsof >/dev/null && lsof "$PORT" >/dev/null 2>&1; then
        die 22 "PORT_BUSY: $PORT is open in another program ($(lsof -t "$PORT" | head -1 | xargs ps -o comm= -p 2>/dev/null)). Close it and run this again."
    fi

    # 2. The image: prebuilt, or build it.
    if [ -z "$IMAGE" ]; then
        export_idf || die 27 "NO_ESP_IDF: ESP-IDF v6.0.3 not found. Install it with ./install.sh --with-firmware-tools (ask the human first), or pass --image with a prebuilt image."
        B=build
        if [ $PUBLIC = 1 ]; then export TIMMY_EXTRA_COMPONENTS=""; B=build
        elif [ -z "${TIMMY_EXTRA_COMPONENTS+x}" ] && [ -d "$TIMMY_HOME/firmware-extras" ]; then B=build-extras; fi
        emit building "Building the firmware ($B)..."
        if [ $DRY = 0 ]; then
            (cd "$ROOT/firmware" && idf.py -B "$B" build) > "${TMPDIR:-/tmp}/timmy-build.log" 2>&1 \
                || die 23 "BUILD_FAILED: see ${TMPDIR:-/tmp}/timmy-build.log"
        fi
        IMAGE="$ROOT/firmware/$B"
    fi
    [ $DRY = 1 ] || [ -f "$IMAGE/flash_args" ] || die 24 "FLASH_FAILED: no flash_args in $IMAGE"

    # 3. Ask, back up, flash.
    if [ $YES = 0 ] && [ $DRY = 0 ]; then
        if [ -t 0 ] && [ $JSON = 0 ]; then
            read -r -p "Flash Timmy firmware from $IMAGE to the board on $PORT? [y/N] " a
            case $a in y|Y|yes) ;; *) die 26 "DECLINED: nothing flashed" ;; esac
        else
            die 26 "DECLINED: flashing needs the human's OK; run again with --yes once they agree"
        fi
    fi
    ESPTOOL="python3 -m esptool"
    export_idf >/dev/null 2>&1 && ESPTOOL="python -m esptool"
    if [ -n "$BACKUP" ]; then
        emit backing_up "Saving the current firmware to $BACKUP (about a minute)..."
        [ $DRY = 1 ] || $ESPTOOL --chip esp32s3 -p "$PORT" -b 921600 read-flash 0 0x822000 "$BACKUP" >/dev/null 2>&1 \
            || die 28 "BACKUP_FAILED: couldn't read the board's flash (nothing was written)"
    fi
    emit flashing "Flashing $IMAGE to $PORT..."
    if [ $DRY = 0 ]; then
        (cd "$IMAGE" && $ESPTOOL --chip esp32s3 -p "$PORT" -b 460800 --before default-reset --after hard-reset write-flash "@flash_args") \
            > "${TMPDIR:-/tmp}/timmy-flash.log" 2>&1 || die 24 "FLASH_FAILED: see ${TMPDIR:-/tmp}/timmy-flash.log (try download mode: hold BOOT, tap reset, release BOOT)"
    fi
    emit flashed "Flashed. Board restarting..."
fi

# 4. Wait for the board to reach the server.
[ $WAIT = 1 ] && [ $DRY = 0 ] || exit 0
STATUS="$TIMMY_HOME/gadget-status.json"
since=$(date +%s)
emit waiting_for_wifi "Connect it to Wi-Fi: on the board's screen open Settings > Wi-Fi and pick your network (2.4 GHz). Waiting for it to reach Timmy's server..."
hinted=0
while :; do
    latest=$(python3 -c 'import json,sys
try: d=json.load(open(sys.argv[1]))
except Exception: d={}
best=max(d.items(), key=lambda kv: kv[1].get("epoch",0), default=None)
print("%s %s %s" % (best[1].get("epoch",0), best[0], best[1].get("firmware","")) if best else "0")' "$STATUS")
    if [ "${latest%% *}" -ge "$since" ] 2>/dev/null; then
        set -- $latest
        emit connected "Connected: board '$2' reached the server (firmware ${3:-unknown})."
        exit 0
    fi
    waited=$(( $(date +%s) - since ))
    if [ $hinted = 0 ] && [ $waited -ge 90 ]; then
        hinted=1
        emit waiting_for_wifi "Not connected yet. Check: the board and this Mac are on the same Wi-Fi (2.4 GHz); the board has the server address and key (>timmy over USB serial, see firmware/README.md); macOS asked to allow incoming connections for Python (System Settings > Network > Firewall)."
    fi
    [ $waited -ge "$WAIT_CONNECT_S" ] && die 25 "NOT_CONNECTED: flashed, but the board didn't reach the server within ${WAIT_CONNECT_S}s (see the hints above; ./install.sh doctor shows the status)."
    sleep 3
done
