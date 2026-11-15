#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Timmy voice helper: list Piper English voices, switch Hermes' TTS voice, play a test line.

  timmy_voice.py list            voices with accent, gender and quality levels
  timmy_voice.py current         the voice Hermes uses now
  timmy_voice.py set <name|id>   e.g. "ryan", "alan", "british", "en_GB-cori-high"
  timmy_voice.py test            speak a short line in the current voice
  timmy_voice.py say <text>      speak any text in the current voice (text "-" reads stdin)

Changes go to ~/.hermes/config.yaml (tts.piper.voice), so the terminal and the web page both use them.
"""

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERMES_HOME = Path(os.environ.get("HERMES_HOME", Path.home() / ".hermes"))
HERMES = str(Path.home() / ".local" / "bin" / "hermes")
VOICE_DIR = HERMES_HOME / "cache" / "piper-voices"

# name: (locale, description, qualities best first)
VOICES = {
    "lessac": ("en_US", "US, female, clear (Timmy's original voice)", ["high", "medium", "low"]),
    "amy": ("en_US", "US, female", ["medium", "low"]),
    "kristin": ("en_US", "US, female", ["medium"]),
    "ljspeech": ("en_US", "US, female, audiobook style", ["high", "medium"]),
    "hfc_female": ("en_US", "US, female", ["medium"]),
    "kathleen": ("en_US", "US, female", ["low"]),
    "ryan": ("en_US", "US, male, deeper", ["high", "medium", "low"]),
    "joe": ("en_US", "US, male", ["medium"]),
    "john": ("en_US", "US, male", ["medium"]),
    "bryce": ("en_US", "US, male", ["medium"]),
    "mike": ("en_US", "US, male", ["medium"]),
    "norman": ("en_US", "US, male, older", ["medium"]),
    "hfc_male": ("en_US", "US, male", ["medium"]),
    "danny": ("en_US", "US, male", ["low"]),
    "sam": ("en_US", "US, non-binary", ["medium"]),
    "alan": ("en_GB", "British, male", ["medium", "low"]),
    "northern_english_male": ("en_GB", "British, male, northern English accent", ["medium"]),
    "cori": ("en_GB", "British, female", ["high", "medium"]),
    "jenny_dioco": ("en_GB", "British, female", ["medium"]),
    "southern_english_female": ("en_GB", "British, female, southern English accent", ["low"]),
    "alba": ("en_GB", "Scottish, female", ["medium"]),
}
# Kokoro (natural-sounding, local, Apache-2.0) voices, served by kokoro_worker.py. Ids are "kokoro:<voice>".
KOKORO = {
    "michael": ("am_michael", "US, male, natural"),
    "adam": ("am_adam", "US, male, natural"),
    "eric": ("am_eric", "US, male, natural"),
    "liam": ("am_liam", "US, male, natural"),
    "onyx": ("am_onyx", "US, male, natural, deep"),
    "fenrir": ("am_fenrir", "US, male, natural"),
    "puck": ("am_puck", "US, male, natural, upbeat"),
    "george": ("bm_george", "British, male, natural"),
    "lewis": ("bm_lewis", "British, male, natural"),
    "fable": ("bm_fable", "British, male, natural"),
    "heart": ("af_heart", "US, female, natural"),
    "bella": ("af_bella", "US, female, natural"),
    "nicole": ("af_nicole", "US, female, natural, soft"),
    "sarah": ("af_sarah", "US, female, natural"),
    "emma": ("bf_emma", "British, female, natural"),
    "isabella": ("bf_isabella", "British, female, natural"),
}
# Orpheus (most human-sounding, slower than real time on this Mac), served by orpheus_worker.py.
ORPHEUS = {
    "leo": ("leo", "US, male, most human"),
    "dan": ("dan", "US, male, most human"),
    "zac": ("zac", "US, male, most human"),
    "tara": ("tara", "US, female, most human"),
    "leah": ("leah", "US, female, most human"),
    "jess": ("jess", "US, female, most human"),
    "mia": ("mia", "US, female, most human"),
    "zoe": ("zoe", "US, female, most human"),
}
ENGINES = {   # engine -> (venv python, worker script, port, Hermes command script)
    "kokoro": (HERMES_HOME / "kokoro-venv" / "bin" / "python", "kokoro_worker.py", 8781, "kokoro_say.py"),
    "orpheus": (HERMES_HOME / "orpheus-venv" / "bin" / "python", "orpheus_worker.py", 8782, "orpheus_say.py"),
}
KOKORO_PY = HERMES_HOME / "kokoro-venv" / "bin" / "python"
SCRIPTS = Path(__file__).resolve().parent

ALIASES = {
    "british": "alan", "uk": "alan", "english": "alan", "british male": "alan",
    "british female": "cori", "scottish": "alba", "northern": "northern_english_male",
    "southern": "southern_english_female", "jenny": "jenny_dioco", "original": "lessac", "default": "lessac",
    "american": "lessac", "american male": "ryan", "male": "ryan", "female": "lessac", "deep": "ryan",
}


def python():
    return sys.executable


def resolve(want):
    if want.startswith("kokoro:"):
        return want if want[7:] in {v for v, _ in KOKORO.values()} else None
    if want.startswith("orpheus:"):
        return want if want[8:] in ORPHEUS else None
    w = want.strip().lower().replace("-", " ").removesuffix(" voice").strip()
    bare = " ".join(p for p in w.split() if p not in ("the", "natural", "kokoro"))
    if bare in KOKORO:
        return "kokoro:" + KOKORO[bare][0]
    bare = bare.replace("orpheus", "").strip()
    if bare in ORPHEUS:
        return "orpheus:" + bare
    if want.startswith(("en_US-", "en_GB-")):
        name, quality = want.split("-", 1)[1].rsplit("-", 1)
        if name in VOICES and quality in VOICES[name][2]:
            return want
        return None
    parts = w.split()
    quality = next((q for q in ("high", "medium", "low") if q in parts), None)
    w = " ".join(p for p in parts if p not in ("high", "medium", "low", "the", "quality"))
    name = ALIASES.get(w, w.replace(" ", "_"))
    if name not in VOICES:
        return None
    locale, _, qualities = VOICES[name]
    return f"{locale}-{name}-{quality if quality in qualities else qualities[0]}"


def _get(key):
    out = subprocess.run([HERMES, "config", "get", key], capture_output=True, text=True).stdout.strip()
    return out.splitlines()[-1].strip().strip('"') if out else ""


def current():
    provider = _get("tts.provider")
    if provider == "kokoro":
        return "kokoro:" + (_get("tts.providers.kokoro.voice") or "am_michael")
    if provider == "orpheus":
        return "orpheus:" + (_get("tts.providers.orpheus.voice") or "leo")
    return _get("tts.piper.voice") or "unknown"


def set_voice(voice_id, length_scale=None):
    """Make voice_id Hermes' TTS voice (terminal, web page, scheduled speech). length_scale > 1 = slower."""
    def cset(k, v):
        # --force: `hermes config set` doesn't know the documented tts.providers.<name> keys yet
        subprocess.run([HERMES, "config", "set", k, str(v), "--force"], check=True, capture_output=True, timeout=60)
    engine = voice_id.split(":", 1)[0] if ":" in voice_id else "piper"
    if engine in ENGINES:
        py, _, _, say = ENGINES[engine]
        cset(f"tts.providers.{engine}.type", "command")
        cset(f"tts.providers.{engine}.command",
             f'"{py}" -I "{SCRIPTS / say}" --in {{input_path}} --out {{output_path}} --voice {{voice}}')
        cset(f"tts.providers.{engine}.output_format", "wav")
        cset(f"tts.providers.{engine}.timeout", 300)
        cset(f"tts.providers.{engine}.voice", voice_id.split(":", 1)[1])
        if length_scale and engine == "kokoro":   # Orpheus has no speed control
            cset("tts.providers.kokoro.speed", round(1 / float(length_scale), 2))
        cset("tts.provider", engine)
    else:
        cset("tts.piper.voice", voice_id)
        if length_scale:
            cset("tts.piper.length_scale", round(float(length_scale), 2))
        cset("tts.provider", "piper")


def ensure_model(voice_id):
    if voice_id.startswith(("kokoro:", "orpheus:")):
        return   # Kokoro's voices ship in one file, already downloaded
    VOICE_DIR.mkdir(parents=True, exist_ok=True)
    if not (VOICE_DIR / f"{voice_id}.onnx").exists():
        print(f"Downloading {voice_id} (one time)...", flush=True)
        subprocess.run([python(), "-m", "piper.download_voices", "--download-dir", str(VOICE_DIR), voice_id],
                       check=True, capture_output=True, timeout=600)


def speak(voice_id, text):
    with tempfile.TemporaryDirectory() as d:
        wav = Path(d) / "test.wav"
        engine = voice_id.split(":", 1)[0] if ":" in voice_id else "piper"
        if engine in ENGINES:
            py, _, _, say = ENGINES[engine]
            src = Path(d) / "in.txt"
            src.write_text(text)
            subprocess.run([str(py), "-I", str(SCRIPTS / say), "--in", str(src), "--out", str(wav),
                            "--voice", voice_id.split(":", 1)[1]], check=True, capture_output=True, timeout=600)
        else:
            subprocess.run([python(), "-m", "piper", "-m", str(VOICE_DIR / f"{voice_id}.onnx"), "-f", str(wav)],
                           input=text.encode(), check=True, capture_output=True, timeout=300)
        subprocess.run(["afplay", str(wav)], check=False, timeout=600)


def main(argv):
    cmd = argv[0] if argv else "list"
    if cmd == "list":
        now = current()
        for name, (locale, desc, qualities) in VOICES.items():
            mark = "  <- current" if now.split("-")[1:2] == [name] else ""
            print(f"{name:24} {desc:42} quality: {', '.join(qualities)}{mark}")
        for name, (vid, desc) in KOKORO.items():
            mark = "  <- current" if now == "kokoro:" + vid else ""
            print(f"{name:24} {desc:42} engine: Kokoro{mark}")
        for name, (vid, desc) in ORPHEUS.items():
            mark = "  <- current" if now == "orpheus:" + vid else ""
            print(f"{name:24} {desc:42} engine: Orpheus (slower){mark}")
        print("\nSay e.g. 'switch to michael' (natural), 'switch to leo' (most human, slower), or 'switch to ryan'.")
    elif cmd == "current":
        print(current())
    elif cmd == "set" and len(argv) > 1:
        voice_id = resolve(" ".join(argv[1:]))
        if not voice_id:
            print(json.dumps({"ok": False, "error": f"unknown voice: {' '.join(argv[1:])}", "voices": list(VOICES)}))
            return 1
        ensure_model(voice_id)
        set_voice(voice_id)
        speak(voice_id, "Hi, this is my new voice. How do I sound?")
        print(json.dumps({"ok": True, "voice": voice_id}))
    elif cmd == "test":
        voice_id = current()
        ensure_model(voice_id)
        speak(voice_id, "Hi, this is Timmy.")
        print(json.dumps({"ok": True, "voice": voice_id}))
    elif cmd == "say" and len(argv) > 1:
        text = sys.stdin.read() if argv[1] == "-" else " ".join(argv[1:])
        voice_id = current()
        ensure_model(voice_id)
        speak(voice_id, text.replace("*", "").replace("#", ""))
        print(json.dumps({"ok": True, "voice": voice_id}))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
