# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Private add-ons: $TIMMY_HOME/addons/<name>/addon.py (TIMMY_HOME defaults to ~/.timmy).

Each add-on module may define register(api), called once when the web server starts. api has:
  api.server   the web server module (timmy_server)
  api.gadget   the desk gadget endpoint module (gadget_server), or None when it isn't running
  api.home     the TIMMY_HOME path
  api.log(msg) write a line to the server's log
An add-on can, for example, append a function to api.gadget.FALLBACK_PROVIDERS that returns the
gadget's fallback wss:// URL. A failing add-on is logged and skipped; the server keeps running.
Add-ons live outside the repository (docs/CUSTOMIZING.md).
"""

import importlib.util
import os
import sys
import types
from pathlib import Path

TIMMY_HOME = Path(os.environ.get("TIMMY_HOME") or Path.home() / ".timmy")
loaded = []


def _log(msg):
    print(f"timmy-addons: {msg}", file=sys.stderr, flush=True)


def load(server, gadget=None):
    folder = TIMMY_HOME / "addons"
    if not folder.is_dir():
        return []
    api = types.SimpleNamespace(server=server, gadget=gadget, home=TIMMY_HOME, log=_log)
    for path in sorted(folder.glob("*/addon.py")):
        name = path.parent.name
        try:
            spec = importlib.util.spec_from_file_location(f"timmy_addon_{name}", path)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            if hasattr(mod, "register"):
                mod.register(api)
            loaded.append(name)
            _log(f"loaded {name}")
        except Exception as exc:
            _log(f"{name} failed: {exc!r}")
    return loaded
