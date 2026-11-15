#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Render Timmy's eyes (avatar/timmy_eyes.c) on this computer into looping GIFs for the docs.

  python3 tools/timmy/make_eye_gifs.py [OUT_DIR]      (default: ../docs/media)
Builds tools/timmy/eyes_preview.c with the host C compiler, renders each scene and writes
eyes-<scene>.gif (and eyes-ring.png, a still). Needs Pillow. No board needed.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw

FW = Path(__file__).resolve().parents[2]
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else FW.parent / "docs" / "media"
FPS = 20
SCENES = {   # name: (seconds, size in px)
    "hero": (10, 240),
    "idle": (6, 280),
    "listening": (4, 280),
    "thinking": (4, 280),
    "speaking": (4, 280),
    "happy": (5, 280),
    "sleepy": (6, 280),
}


def build():
    exe = Path(tempfile.gettempdir()) / "timmy_eyes_preview"
    subprocess.run(["cc", "-O2", "-I", str(FW / "components/timmy"), str(FW / "tools/timmy/eyes_preview.c"),
                    str(FW / "avatar/timmy_eyes.c"), "-lm", "-o", str(exe)], check=True)
    return exe


def frames(exe, scene, size, n):
    raw = subprocess.run([str(exe), scene, str(size), str(n), str(FPS)], check=True, capture_output=True).stdout
    step = size * size * 3
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, size - 1, size - 1), fill=255)   # the round screen
    black = Image.new("RGB", (size, size), (0, 0, 0))
    return [Image.composite(Image.frombytes("RGB", (size, size), raw[i * step:(i + 1) * step]), black, mask)
            for i in range(n)]


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    exe = build()
    for scene, (secs, size) in SCENES.items():
        imgs = frames(exe, scene, size, secs * FPS)
        pal = [im.convert("P", palette=Image.Palette.ADAPTIVE, colors=96 if scene == "hero" else 128) for im in imgs]
        path = OUT / f"eyes-{scene}.gif"
        pal[0].save(path, save_all=True, append_images=pal[1:], duration=int(1000 / FPS), loop=0, optimize=True,
                    disposal=1)
        print(f"{path.name}: {path.stat().st_size / 1e6:.2f} MB")
    ring = frames(exe, "ring", 360, 30)[-1]
    ring.save(OUT / "eyes-ring.png", optimize=True)
    print(f"eyes-ring.png: {(OUT / 'eyes-ring.png').stat().st_size / 1e6:.2f} MB")


if __name__ == "__main__":
    main()
