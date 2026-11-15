#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
#
# One-command installer for Timmy on macOS.
#
#   ./install.sh [--yes] [--dry-run] [--json] [--no-services] [--no-open] [--with-firmware-tools]
#   ./install.sh doctor [--json]          check everything, print a summary (or JSON)
#   ./install.sh flash [flash.sh options] guided firmware flash (scripts/flash.sh)
#
# Steps (each is skipped when already done, so re-running is safe):
#   1. Homebrew and Python 3          (brew install python@3.12 when python3 is missing)
#   2. Hermes Agent                   (Nous Research's installer, when `hermes` is missing)
#   3. ~/.timmy (TIMMY_HOME)          created from templates/timmy-home if missing; an existing one is never touched
#   4. Voice                          the default Piper voice, downloaded once
#   5. Gateway and web page services  Hermes gateway login service + the web page hook (skip: --no-services)
#   6. Private add-ons                listed from $TIMMY_HOME/addons (the server loads them at start)
#   7. Firmware tools (optional)      ESP-IDF v6.0.3, only with --with-firmware-tools
#   8. First-run setup page           http://localhost:8000/setup.html, where YOU enter the API key
#
# Non-secret options can come from flags or the environment: TIMMY_HOME, TIMMY_PORT, TIMMY_VOICE.
# Secrets are never taken on the command line: the setup page asks the human for them.
#
# Exit codes: 0 ok, 2 usage, 10 NOT_MACOS, 11 HOMEBREW_MISSING, 12 PYTHON_FAILED, 13 HERMES_FAILED,
#             14 TIMMY_HOME_FAILED, 15 VOICE_FAILED, 16 SERVICES_FAILED, 17 WEB_NOT_UP, 18 ESP_IDF_FAILED
set -uo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"

case "${1:-}" in
    doctor) shift; exec "$ROOT/scripts/doctor.sh" "$@" ;;
    flash) shift; exec "$ROOT/scripts/flash.sh" "$@" ;;
esac

YES=0 DRY=0 JSON=0 SERVICES=1 OPEN=1 FWTOOLS=0
while [ $# -gt 0 ]; do
    case $1 in
        --yes|-y) YES=1 ;;
        --dry-run) DRY=1 ;;
        --json) JSON=1 ;;
        --no-services) SERVICES=0 ;;
        --no-open) OPEN=0 ;;
        --with-firmware-tools) FWTOOLS=1 ;;
        -h|--help) sed -n 4,27p "$0"; exit 0 ;;
        *) echo "unknown option $1 (see ./install.sh --help)" >&2; exit 2 ;;
    esac
    shift
done
export TIMMY_HOME=${TIMMY_HOME:-$HOME/.timmy}
PORT=${TIMMY_PORT:-8000}
VOICE=${TIMMY_VOICE:-en_US-lessac-medium}
HERMES_INSTALL_URL=${HERMES_INSTALL_URL:-https://hermes-agent.nousresearch.com/install.sh}
export PATH="$HOME/.local/bin:/opt/homebrew/bin:/usr/local/bin:$PATH"

step=0
log() {   # log STATE MESSAGE
    if [ $JSON = 1 ]; then
        python3 -c 'import json,sys; print(json.dumps({"step": int(sys.argv[1]), "state": sys.argv[2], "message": sys.argv[3]}), flush=True)' "$step" "$1" "$2" 2>/dev/null \
            || printf '{"step": %s, "state": "%s", "message": "%s"}\n' "$step" "$1" "$2"
    else
        case $1 in
            start) printf '\n[%s] %s\n' "$step" "$2" ;;
            ok) printf '    ✓ %s\n' "$2" ;;
            skip) printf '    - %s\n' "$2" ;;
            dry) printf '    (dry run) %s\n' "$2" ;;
            fail) printf '    ✗ %s\n' "$2" ;;
            *) printf '    %s\n' "$2" ;;
        esac
    fi
}
fail() { log fail "$2"; exit "$1"; }
run() {   # run CMD... (or show it in a dry run)
    if [ $DRY = 1 ]; then log dry "$*"; return 0; fi
    "$@"
}
confirm() {   # confirm QUESTION: yes with --yes; ask on a terminal; no otherwise
    [ $YES = 1 ] && return 0
    [ $DRY = 1 ] && return 0
    if [ -t 0 ]; then read -r -p "    $1 [y/N] " a; case $a in y|Y|yes) return 0;; esac; fi
    return 1
}

step=1; log start "Homebrew and Python"
[ "$(uname -s)" = Darwin ] || fail 10 "NOT_MACOS: this installer is for macOS"
if command -v brew >/dev/null 2>&1; then log ok "Homebrew: $(brew --version | head -1)"
else
    log info "Homebrew is missing. Installing it needs an administrator password, so run this yourself:"
    log info '/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"'
    [ $DRY = 1 ] || fail 11 "HOMEBREW_MISSING: install Homebrew (command above), then run ./install.sh again"
fi
if command -v python3 >/dev/null 2>&1 && python3 -c 'import sys; sys.exit(sys.version_info < (3, 10))'; then
    log ok "Python: $(python3 --version)"
else
    run brew install python@3.12 || fail 12 "PYTHON_FAILED: brew install python@3.12 failed"
    log ok "Python installed"
fi

step=2; log start "Hermes Agent (the brain, speech-to-text and text-to-speech)"
if command -v hermes >/dev/null 2>&1; then log ok "$(hermes --version 2>/dev/null | head -1)"
else
    if confirm "Install Hermes Agent from $HERMES_INSTALL_URL?"; then
        if [ $DRY = 1 ]; then log dry "curl -fsSL $HERMES_INSTALL_URL | bash"
        else curl -fsSL "$HERMES_INSTALL_URL" | bash || fail 13 "HERMES_FAILED: the Hermes Agent installer failed (see its output)"
        fi
        log ok "Hermes Agent installed"
    else
        fail 13 "HERMES_FAILED: Hermes Agent is required (run with --yes, or install it yourself: curl -fsSL $HERMES_INSTALL_URL | bash)"
    fi
fi

step=3; log start "Personal data folder: $TIMMY_HOME"
if [ -d "$TIMMY_HOME" ]; then
    log ok "exists; left untouched"
else
    run mkdir -p "$TIMMY_HOME" || fail 14 "TIMMY_HOME_FAILED: can't create $TIMMY_HOME"
    run cp -R "$ROOT/templates/timmy-home/." "$TIMMY_HOME/" || fail 14 "TIMMY_HOME_FAILED: can't copy templates"
    run cp "$ROOT/.env.example" "$TIMMY_HOME/.env"
    run mkdir -p "$TIMMY_HOME/keys" "$TIMMY_HOME/briefings" "$TIMMY_HOME/topics" "$TIMMY_HOME/addons" "$TIMMY_HOME/skill-state"
    run chmod 700 "$TIMMY_HOME" "$TIMMY_HOME/keys"
    run chmod 600 "$TIMMY_HOME/.env"
    log ok "created from templates (keys/ and .env are private: mode 700/600)"
fi

step=4; log start "Voice ($VOICE)"
HPY=$(ls -td "$HOME"/.hermes/installs/*/environments/*/venv 2>/dev/null | head -1)/bin/python
if [ ! -x "$HPY" ]; then
    [ $DRY = 1 ] && log dry "would use Hermes' Python to download $VOICE" || log skip "Hermes' Python not found yet; the voice downloads on first use"
else
    if [ $DRY = 1 ]; then log dry "download $VOICE with Hermes' Python"
    else
        "$HPY" -c "import sys; sys.path.insert(0, '$ROOT/server/hermes-skills/voices/scripts'); import timmy_voice; timmy_voice.ensure_model('$VOICE')" \
            >/dev/null 2>&1 && log ok "voice ready" || fail 15 "VOICE_FAILED: couldn't download $VOICE (network?). Re-run later; Timmy works with the browser's voice meanwhile."
    fi
fi

step=5; log start "Services: Hermes gateway and Timmy's web page"
if [ $SERVICES = 0 ]; then log skip "skipped (--no-services); start by hand with ./start-timmy.sh"
elif confirm "Set Timmy to start at login (Hermes gateway login service + web page hook) and start it now?"; then
    run "$ROOT/server/autostart/install.sh" || fail 16 "SERVICES_FAILED: server/autostart/install.sh failed (logs: ~/.hermes/logs/)"
    log ok "services installed"
else
    log skip "not installed (no consent); start by hand with ./start-timmy.sh"
    SERVICES=0
fi

step=6; log start "Private add-ons"
if [ -d "$TIMMY_HOME/addons" ] && ls "$TIMMY_HOME/addons"/*/addon.py >/dev/null 2>&1; then
    for a in "$TIMMY_HOME"/addons/*/addon.py; do log ok "add-on: $(basename "$(dirname "$a")") (loaded when the server starts)"; done
else
    log skip "none in $TIMMY_HOME/addons"
fi
[ -d "$TIMMY_HOME/firmware-extras" ] && log ok "firmware add-ons found: scripts/flash.sh builds with them"

step=7; log start "Firmware tools (ESP-IDF v6.0.3)"
if [ $FWTOOLS = 0 ]; then log skip "skipped (only needed to build firmware: --with-firmware-tools)"
elif [ -f "$HOME/esp/esp-idf-v6.0.3/export.sh" ] || [ -f "${IDF_PATH:-/nonexistent}/export.sh" ]; then log ok "ESP-IDF found"
else
    run brew install cmake ninja dfu-util || fail 18 "ESP_IDF_FAILED: brew install cmake ninja dfu-util failed"
    run git clone -b v6.0.3 --recursive --depth 1 --shallow-submodules https://github.com/espressif/esp-idf.git "$HOME/esp/esp-idf-v6.0.3" || fail 18 "ESP_IDF_FAILED: clone failed"
    run "$HOME/esp/esp-idf-v6.0.3/install.sh" esp32s3 || fail 18 "ESP_IDF_FAILED: install.sh esp32s3 failed"
    log ok "ESP-IDF installed in ~/esp/esp-idf-v6.0.3"
fi

step=8; log start "First-run setup"
URL="http://localhost:$PORT/setup.html"
if [ $SERVICES = 1 ] && [ $DRY = 0 ]; then
    for _ in $(seq 1 30); do curl -sf "http://localhost:$PORT/setup/status" >/dev/null && break; sleep 2; done
    curl -sf "http://localhost:$PORT/setup/status" >/dev/null || fail 17 "WEB_NOT_UP: the web page didn't come up (./install.sh doctor; logs in ~/.hermes/logs/)"
fi
log info "Open $URL and enter the Claude API key there yourself (or choose a local model) and pick a voice."
[ $OPEN = 1 ] && [ $DRY = 0 ] && [ $SERVICES = 1 ] && open "$URL" 2>/dev/null
log ok "done. Check everything with: ./install.sh doctor   Flash the board with: ./install.sh flash"
exit 0
