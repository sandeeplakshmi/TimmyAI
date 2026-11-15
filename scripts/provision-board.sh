#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
#
# Point the desk gadget at this Mac's Timmy server over USB serial, without showing the device key.
#   scripts/provision-board.sh [--port PORT] [--host ADDRESS] [--device NAME] [--new-key]
# Makes a device key for NAME (default timmy-desk) in $TIMMY_HOME/keys/gadget-keys.json if it has none
# (or a fresh one with --new-key), then sends >timmy.host, >timmy.device and >timmy.key to the board.
# The key goes from the key file straight to the board; it is never printed. Restart the web server
# afterwards if this was the first key (the gadget endpoint starts only when a key exists).
# Exit: 0 ok, 2 usage, 20 BOARD_NOT_FOUND, 21 MULTIPLE_BOARDS, 29 NO_PYSERIAL, 30 BOARD_DIDNT_ANSWER
set -uo pipefail
PY=""
for p in "${IDF_PYTHON_ENV_PATH:-}/bin/python" "$HOME"/.espressif/python_env/*/bin/python python3; do
    [ -x "$p" ] || command -v "$p" >/dev/null 2>&1 || continue
    "$p" -c "import serial" 2>/dev/null && { PY=$p; break; }
done
[ -n "$PY" ] || { echo "NO_PYSERIAL: needs pyserial (python3 -m pip install --user pyserial)" >&2; exit 29; }
exec "$PY" - "$@" <<'PYEOF'
import json, os, secrets, subprocess, sys, time
from pathlib import Path
import serial
from serial.tools import list_ports

args = sys.argv[1:]
def opt(name, default=None):
    if name in args:
        i = args.index(name)
        return args[i + 1] if i + 1 < len(args) else sys.exit(2)
    return default
port, host, device = opt("--port"), opt("--host"), opt("--device", "timmy-desk")
home = Path(os.environ.get("TIMMY_HOME", Path.home() / ".timmy"))
keys_file = home / "keys" / "gadget-keys.json"

if not port:
    ports = [p.device.replace("/dev/tty.", "/dev/cu.") for p in list_ports.comports() if p.vid == 0x303A]
    if not ports:
        print("BOARD_NOT_FOUND: plug the board in with a USB-C data cable (no hub) and run this again"); sys.exit(20)
    if len(ports) > 1:
        print("MULTIPLE_BOARDS: " + " ".join(ports) + " - pass --port"); sys.exit(21)
    port = ports[0]
if not host:
    for ifc in ("en0", "en1"):
        host = subprocess.run(["ipconfig", "getifaddr", ifc], capture_output=True, text=True).stdout.strip()
        if host:
            break
if not host:
    print("couldn't find this Mac's LAN address; pass --host"); sys.exit(2)

try:
    keys = json.loads(keys_file.read_text())
except (OSError, ValueError):
    keys = {}
if device not in keys or "--new-key" in args:
    keys[device] = secrets.token_hex(32)
    keys_file.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(keys_file, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(keys, f, indent=1)
    os.chmod(keys_file, 0o600)
    print(f"made a device key for {device} in {keys_file} (not shown)")

s = serial.Serial(port, 115200, timeout=0.3)
s.dtr = False; s.rts = False
def send(line, expect):
    s.reset_input_buffer()
    s.write((">" + line + "\n").encode())
    end, buf = time.time() + 4, b""
    while time.time() < end:
        buf += s.read(512)
        if expect.encode() in buf:
            return True
    return False
ok = (send(f"timmy.host={host}", "@timmy.url ok") and send(f"timmy.device={device}", "@timmy.device ok")
      and send("timmy.key=" + keys[device], "@timmy.key ok"))
s.close()
if not ok:
    print("BOARD_DIDNT_ANSWER: the board didn't confirm the settings. Is the Timmy firmware on it, and no other serial monitor open?")
    sys.exit(30)
print(f"✓ board on {port} now points at ws://{host}:8790/gadget as {device}. If this was the first key, restart the web server.")
PYEOF
