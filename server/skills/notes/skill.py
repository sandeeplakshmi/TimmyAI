# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Quick notes in a local Markdown file: one '- YYYY-MM-DD HH:MM  text' line each."""

import re
from pathlib import Path


def _file(kit):
    f = kit.config.get("file")
    return Path(f).expanduser() if f else kit.data_dir.parent / "notes.md"


def handle(kit, req):
    path = _file(kit)
    note = req.match.get("note")
    if note:
        note = note.strip().rstrip(".")
        path.parent.mkdir(parents=True, exist_ok=True)
        new = not path.exists()
        with open(path, "a") as f:
            if new:
                f.write("# Notes\n\n")
            f.write(f"- {kit.now():%Y-%m-%d %H:%M}  {note}\n")
        kit.eyes("noted", secs=3)
        return f"Noted: {note}."
    lines = [l for l in path.read_text().splitlines() if l.startswith("- ")] if path.exists() else []
    if not lines:
        return "You don't have any notes yet."
    n = int(kit.config.get("read_last", 3))
    last = [re.sub(r"^- \S+ \S+\s+", "", l) for l in lines[-n:]]
    return f"Your last {len(last)} note{'s' * (len(last) != 1)}: " + "; ".join(last) + "."
