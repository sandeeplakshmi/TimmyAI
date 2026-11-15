#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Hermes command-type TTS provider for Kokoro (terminal voice mode, cron speech, the tts tool).

  kokoro_say.py --in <text file> --out <wav file> [--voice am_michael]

Uses the running Kokoro helper (kokoro_worker.py on 127.0.0.1:8781) when it's up, otherwise loads the
model itself (slower first reply). Speed comes from tts.providers.kokoro.speed in ~/.hermes/config.yaml.
"""

import argparse
import json
import re
import urllib.request
from pathlib import Path

HOME = Path.home() / ".hermes"


def config_speed():
    try:
        text = (HOME / "config.yaml").read_text()
        block = text.split("kokoro:", 1)[1][:600]
        m = re.search(r"^\s+speed:\s*([0-9.]+)", block, re.M)
        return float(m.group(1)) if m else 1.0
    except (OSError, IndexError, ValueError):
        return 1.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--voice", default="am_michael")
    a = ap.parse_args()
    text = Path(a.inp).read_text(encoding="utf-8").strip()
    speed = config_speed()
    try:
        req = urllib.request.Request("http://127.0.0.1:8781/speak", method="POST",
                                     data=json.dumps({"text": text, "voice": a.voice or "am_michael", "speed": speed}).encode(),
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=120) as r:
            Path(a.out).write_bytes(r.read())
        return
    except OSError:
        pass
    import soundfile as sf   # helper not running: synthesize here
    from kokoro_onnx import Kokoro
    k = Kokoro(str(HOME / "cache/kokoro/kokoro-v1.0.onnx"), str(HOME / "cache/kokoro/voices-v1.0.bin"))
    voice = a.voice or "am_michael"
    audio, rate = k.create(text, voice=voice, speed=speed, lang="en-gb" if voice.startswith(("bf_", "bm_")) else "en-us")
    sf.write(a.out, audio, rate, format="WAV", subtype="PCM_16")


if __name__ == "__main__":
    main()
