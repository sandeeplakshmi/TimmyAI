#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
#
# Make Timmy start automatically at login (run this yourself; it adds persistent services).
#   1. Hermes gateway (brain, API server, scheduled jobs) via Hermes' own login service
#   2. Timmy web page (http://localhost:8000), served from inside the gateway by a gateway hook
#      (~/.hermes/hooks/timmy-web/). Separate login items can't read ~/Documents because of macOS
#      privacy rules; the gateway can.
# Safe to run again. Undo with: server/autostart/uninstall.sh
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
export PATH="$HOME/.local/bin:$PATH"
gateway_up() { curl -sf http://127.0.0.1:8642/health >/dev/null; }

mkdir -p "$HOME/.hermes/hooks/timmy-web" "$HOME/.hermes/logs"
cp "$REPO/server/autostart/hook/HOOK.yaml" "$REPO/server/autostart/hook/handler.py" "$HOME/.hermes/hooks/timmy-web/"
printf '%s\n' "$REPO" > "$HOME/.hermes/hooks/timmy-web/timmy_repo.txt"

# Stop a hand-started web server so the gateway's copy can take port 8000
pkill -f "server/timmy_server.py" || true

hermes gateway install --start-on-login --start-now --if-missing
if gateway_up; then hermes gateway restart; else hermes gateway start; fi
for _ in $(seq 1 45); do gateway_up && break; sleep 2; done

echo "Waiting for Timmy to come up..."
for _ in $(seq 1 45); do curl -sf http://localhost:8000/voice/status >/dev/null && break; sleep 2; done
gateway_up && echo "✓ Hermes gateway is running" || echo "✗ gateway not up; run: hermes gateway start  (logs: ~/.hermes/logs/)"
curl -sf http://localhost:8000/voice/status >/dev/null && echo "✓ Timmy web page is up: http://localhost:8000" || echo "✗ web page not up; see ~/.hermes/logs/timmy-web.out"
