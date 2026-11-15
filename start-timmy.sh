#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
#
# Start Timmy: the Hermes gateway (API server on 127.0.0.1:8642) and the web HUD on http://localhost:8000.
#   ./start-timmy.sh              start what's missing and open the page
#   ./start-timmy.sh --no-open    same, without opening the browser
#   ./start-timmy.sh --web-only   only the web server; wait for the gateway (used by the login item,
#                               since the gateway has its own login service)
# Ctrl+C stops the web server; the gateway keeps running (stop it with: hermes gateway stop).
set -e
cd "$(dirname "$0")"
# Personal settings (.env) live outside the repo, in $TIMMY_HOME (default ~/.timmy).
export TIMMY_HOME="${TIMMY_HOME:-$HOME/.timmy}"
[ -f "$TIMMY_HOME/.env" ] && { set -a; . "$TIMMY_HOME/.env"; set +a; }
export PATH="$HOME/.local/bin:$PATH"
health() { curl -sf http://127.0.0.1:8642/health >/dev/null; }

if [[ " $* " == *" --web-only "* ]]; then
  for _ in $(seq 1 120); do health && break; sleep 2; done
elif ! health; then
  echo "Starting Hermes gateway..."
  mkdir -p "$HOME/.hermes/logs"
  nohup hermes gateway run >"$HOME/.hermes/logs/timmy-gateway.out" 2>&1 &
  for _ in $(seq 1 30); do health && break; sleep 1; done
fi
health || { echo "Gateway is not running; see ~/.hermes/logs/"; exit 1; }

# Use Hermes' own Python so the page gets the same voice (Whisper + Piper) as the terminal.
PY=$(ls -td "$HOME"/.hermes/installs/*/environments/*/venv 2>/dev/null | head -1)/bin/python
[ -x "$PY" ] || PY=python3
[[ " $* " == *" --no-open "* || " $* " == *" --web-only "* ]] || (sleep 1; open http://localhost:8000) &
exec "$PY" server/timmy_server.py
