#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Kokoro TTS helper for Timmy: keeps the Kokoro model loaded and serves speech on 127.0.0.1:8781.

Runs with ~/.hermes/kokoro-venv/bin/python (kokoro-onnx, separate from Hermes' own Python).
  GET  /health                          -> {"ok": true}
  POST /speak  {"text", "voice", "speed"} -> audio/wav
Started on demand by ui/timmy_server.py; kokoro_say.py (the Hermes command provider) uses it too.
Model files: ~/.hermes/cache/kokoro/kokoro-v1.0.onnx + voices-v1.0.bin (Kokoro-82M, Apache-2.0).
"""

import io
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import soundfile as sf
from kokoro_onnx import Kokoro

MODEL_DIR = Path.home() / ".hermes" / "cache" / "kokoro"
PORT = 8781
KOKORO = Kokoro(str(MODEL_DIR / "kokoro-v1.0.onnx"), str(MODEL_DIR / "voices-v1.0.bin"))
LOCK = threading.Lock()   # one synthesis at a time keeps latency predictable


def synth(text, voice="am_michael", speed=1.0):
    lang = "en-gb" if voice.startswith(("bf_", "bm_")) else "en-us"
    with LOCK:
        audio, rate = KOKORO.create(text, voice=voice, speed=float(speed), lang=lang)
    buf = io.BytesIO()
    sf.write(buf, audio, rate, format="WAV", subtype="PCM_16")
    return buf.getvalue()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _send(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            return self._send(200, b'{"ok": true}', "application/json")
        self._send(404, b"{}", "application/json")

    def do_POST(self):
        if self.path != "/speak":
            return self._send(404, b"{}", "application/json")
        try:
            req = json.loads(self.rfile.read(int(self.headers.get("Content-Length") or 0)) or b"{}")
            text = (req.get("text") or "").strip()
            if not text:
                return self._send(400, b'{"error": "no text"}', "application/json")
            self._send(200, synth(text, req.get("voice") or "am_michael", req.get("speed") or 1.0), "audio/wav")
        except Exception as exc:
            self._send(500, json.dumps({"error": str(exc)}).encode(), "application/json")


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
