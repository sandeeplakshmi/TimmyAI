#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Make sure Hermes loads Timmy's brain skills: adds directories to skills.external_dirs in
$HERMES_HOME/config.yaml (default ~/.hermes), keeping the rest of the file (and its comments) as it is.

  scripts/hermes-skill-dirs.py [--dry-run] [--remove DIR] DIR...
Prints what it changed. Exit 0 ok, 1 no config file, 2 couldn't find a place to add them.
"""
import os
import re
import shutil
import sys
import time
from pathlib import Path

args = sys.argv[1:]
dry = "--dry-run" in args
remove = []
dirs = []
i = 0
while i < len(args):
    if args[i] == "--dry-run":
        pass
    elif args[i] == "--remove":
        i += 1
        remove.append(args[i])
    else:
        dirs.append(args[i])
    i += 1
cfg = Path(os.environ.get("HERMES_HOME", Path.home() / ".hermes")) / "config.yaml"
if not cfg.exists():
    print(f"no {cfg}; run Hermes once (hermes setup) first")
    sys.exit(1)
lines = cfg.read_text().split("\n")
# Find "skills:" at column 0, then "  external_dirs:" inside it.
try:
    s = next(k for k, l in enumerate(lines) if re.match(r"^skills:\s*(#.*)?$", l))
except StopIteration:
    lines += ["skills:", "  external_dirs:"]
    s = len(lines) - 2
end = next((k for k in range(s + 1, len(lines)) if lines[k] and not lines[k].startswith((" ", "#"))), len(lines))
e = next((k for k in range(s + 1, end) if re.match(r"^  external_dirs:", lines[k])), None)
if e is None:
    lines.insert(s + 1, "  external_dirs:")
    e = s + 1
    end += 1
if re.match(r"^  external_dirs:\s*\[\s*\]", lines[e]):
    lines[e] = "  external_dirs:"
items_end = e + 1
while items_end < len(lines) and re.match(r"^    (- |#)", lines[items_end]):
    items_end += 1
present = [re.sub(r'^    - "?(.*?)"?\s*(#.*)?$', r"\1", l) for l in lines[e + 1:items_end] if l.startswith("    - ")]
changed = []
for d in remove:
    for k in range(items_end - 1, e, -1):
        if lines[k].startswith("    - ") and d in lines[k]:
            del lines[k]
            items_end -= 1
            changed.append(f"removed {d}")
for d in dirs:
    d = str(Path(d).expanduser())
    if d not in present:
        lines.insert(items_end, f'    - "{d}"   # Timmy brain skills')
        items_end += 1
        changed.append(f"added {d}")
if not changed:
    print("skills.external_dirs already up to date")
    sys.exit(0)
for c in changed:
    print(("(dry run) " if dry else "") + c)
if not dry:
    shutil.copy2(cfg, cfg.with_name(f"config.yaml.bak-timmy-{time.strftime('%Y%m%d-%H%M%S')}"))
    cfg.write_text("\n".join(lines))
