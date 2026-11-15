# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""First-run setup (web/setup.html): the brain's key or a local model, stored outside the repo.

The Claude API key goes to Hermes' own env file ($HERMES_HOME/.env, mode 600), where Hermes Agent reads it.
Non-secret choices (local model, assistant name) go to $TIMMY_HOME/.env. Secrets are never returned to the page.
"""

import os
import re
import secrets
from pathlib import Path

TIMMY_HOME = Path(os.environ.get("TIMMY_HOME") or Path.home() / ".timmy")
HERMES_HOME = Path(os.environ.get("HERMES_HOME", Path.home() / ".hermes"))
KEY_RE = re.compile(r"^sk-ant-[A-Za-z0-9_\-]{20,200}$")
SAFE_VALUE = re.compile(r"^[A-Za-z0-9 _.:/@+\-]{0,200}$")


def _read_env(path):
    out = {}
    try:
        for line in path.read_text().splitlines():
            if "=" in line and not line.lstrip().startswith("#"):
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip().strip('"').strip("'")
    except OSError:
        pass
    return out


def _set_env(path, values):
    """Set KEY=value lines in an env file, keeping the others; mode 600."""
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = path.read_text().splitlines() if path.exists() else []
    done = set()
    for i, line in enumerate(lines):
        k = line.split("=", 1)[0].strip()
        if k in values and not line.lstrip().startswith("#"):
            lines[i] = f"{k}={values[k]}"
            done.add(k)
    lines += [f"{k}={v}" for k, v in values.items() if k not in done]
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write("\n".join(lines) + "\n")
    os.chmod(path, 0o600)


def status():
    h, t = _read_env(HERMES_HOME / ".env"), _read_env(TIMMY_HOME / ".env")
    return {"claude_key_set": bool(h.get("ANTHROPIC_API_KEY")),
            "api_server_ready": h.get("API_SERVER_ENABLED", "").lower() == "true" and bool(h.get("API_SERVER_KEY")),
            "local_model": t.get("TIMMY_LOCAL_MODEL", ""),
            "assistant_name": t.get("TIMMY_USER_NAME", ""),
            "timmy_home": str(TIMMY_HOME)}


def save(body):
    """body: {"brain": "claude"|"local", "claude_key": "...", "local_model": "...", "user_name": "..."}."""
    hermes = {}
    timmy = {}
    brain = body.get("brain")
    if brain == "claude":
        key = str(body.get("claude_key") or "").strip()
        if key and not KEY_RE.match(key):
            return {"ok": False, "error": "That doesn't look like a Claude API key (it starts with sk-ant-)."}
        if key:
            hermes["ANTHROPIC_API_KEY"] = key
        timmy["TIMMY_LOCAL_MODEL"] = ""
    elif brain == "local":
        model = str(body.get("local_model") or "").strip()
        if not model or not SAFE_VALUE.match(model):
            return {"ok": False, "error": "Give a local model name, e.g. qwen2.5:7b (Ollama)."}
        timmy["TIMMY_LOCAL_MODEL"] = model
    name = str(body.get("user_name") or "").strip()
    if name and SAFE_VALUE.match(name):
        timmy["TIMMY_USER_NAME"] = name
    # The web page talks to Hermes through its local API server: make sure it's on, with its own key.
    h = _read_env(HERMES_HOME / ".env")
    if h.get("API_SERVER_ENABLED", "").lower() != "true":
        hermes["API_SERVER_ENABLED"] = "true"
    if not h.get("API_SERVER_KEY"):
        hermes["API_SERVER_KEY"] = secrets.token_hex(24)
    if hermes:
        _set_env(HERMES_HOME / ".env", hermes)
    if timmy:
        _set_env(TIMMY_HOME / ".env", timmy)
    next_steps = ["Restart the gateway so it reads the new settings: hermes gateway restart"]
    if brain == "local":
        next_steps.insert(0, f"Point Hermes at the local model: hermes model (pick Ollama / {timmy['TIMMY_LOCAL_MODEL']})")
    return {"ok": True, "status": status(), "next": next_steps}
