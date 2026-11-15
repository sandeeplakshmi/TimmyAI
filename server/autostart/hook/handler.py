# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Hermes gateway hook: run Timmy's web server (server/timmy_server.py) in a background thread of the gateway.

Installed to ~/.hermes/hooks/timmy-web/ by server/autostart/install.sh, which writes the repo path into
timmy_repo.txt next to this file. Running inside the gateway matters on macOS: the gateway is allowed to read
~/Documents, while separate login items and the processes cron scripts start are blocked.
"""

import importlib.util
import socket
import threading
from pathlib import Path

HERE = Path(__file__).resolve().parent
SERVER = Path((HERE / "timmy_repo.txt").read_text().strip()) / "server" / "timmy_server.py"
LOG = Path.home() / ".hermes" / "logs" / "timmy-web.out"


def _port_busy(port=8000):
    with socket.socket() as s:
        return s.connect_ex(("127.0.0.1", port)) == 0


def _run():
    try:
        spec = importlib.util.spec_from_file_location("timmy_server", SERVER)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        mod.serve()
    except Exception as exc:  # never take the gateway down
        with open(LOG, "a") as f:
            f.write(f"timmy-web hook: web server stopped: {exc!r}\n")


def handle(event_type, context):
    if event_type == "gateway:startup" and not _port_busy():
        threading.Thread(target=_run, name="timmy-web", daemon=True).start()
