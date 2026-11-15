#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Save today's briefing for the web page and optionally speak it.

  publish_briefing.py [--speak] < briefing.txt

Writes $TIMMY_HOME/briefings/ (default ~/.timmy/briefings/)YYYY-MM-DD.md (the web page at http://localhost:8000 shows the newest one).
With --speak, also reads it aloud on the Mac speakers in Timmy's current Piper voice.
"""

import os
import subprocess
import sys
from datetime import date
from pathlib import Path

TIMMY_HOME = Path(os.environ.get("TIMMY_HOME", Path.home() / ".timmy"))
OUT = TIMMY_HOME / "briefings"
VOICE = Path(__file__).resolve().parents[2] / "voices" / "scripts" / "timmy_voice.py"

text = sys.stdin.read().strip()
if not text:
    sys.exit("no briefing text on stdin")
OUT.mkdir(parents=True, exist_ok=True)
path = OUT / f"{date.today().isoformat()}.md"
path.write_text(text + "\n")
print(f"saved {path}")
if "--speak" in sys.argv:
    subprocess.run([sys.executable, str(VOICE), "say", "-"], input=text.encode(), check=False)
