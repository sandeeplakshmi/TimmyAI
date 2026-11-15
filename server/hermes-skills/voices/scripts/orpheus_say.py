#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Hermes command-type TTS provider for Orpheus (terminal voice, spoken briefing, the tts tool).

  orpheus_say.py --in <text file> --out <wav file> [--voice leo]

Uses the running Orpheus helper (orpheus_worker.py on 127.0.0.1:8782) when it's up, otherwise loads the
model itself (much slower first reply).
"""

import argparse
import json
import urllib.request
from pathlib import Path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--voice", default="leo")
    a = ap.parse_args()
    text = Path(a.inp).read_text(encoding="utf-8").strip()
    try:
        req = urllib.request.Request("http://127.0.0.1:8782/speak", method="POST",
                                     data=json.dumps({"text": text, "voice": a.voice or "leo"}).encode(),
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=300) as r:
            Path(a.out).write_bytes(r.read())
        return
    except OSError:
        pass
    import numpy as np   # helper not running: synthesize here
    import soundfile as sf
    from mlx_audio.tts.utils import load_model
    model = load_model("mlx-community/orpheus-3b-0.1-ft-4bit")
    chunks, rate = [], 24000
    for r in model.generate(text=text, voice=a.voice or "leo", verbose=False):
        chunks.append(np.array(r.audio))
        rate = getattr(r, "sample_rate", rate)
    sf.write(a.out, np.concatenate(chunks), rate, format="WAV", subtype="PCM_16")


if __name__ == "__main__":
    main()
