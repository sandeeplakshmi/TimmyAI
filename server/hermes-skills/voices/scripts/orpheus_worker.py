#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Orpheus TTS helper for Timmy: keeps Orpheus 3B (4-bit, MLX on the Apple GPU) loaded on 127.0.0.1:8782.

Runs with ~/.hermes/orpheus-venv/bin/python (mlx-audio). Orpheus sounds the most human but on this Mac
runs at about 0.5-0.7x real time, so replies start later than with Kokoro.
  GET  /health                     -> {"ok": true}
  POST /speak  {"text", "voice"}   -> audio/wav   (voices: leo, dan, zac, tara, leah, jess, mia, zoe)
Model: mlx-community/orpheus-3b-0.1-ft-4bit (Hugging Face cache, downloaded once).
"""

import io
import json
import queue
import threading
from concurrent.futures import Future
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import numpy as np
import soundfile as sf

PORT = 8782
JOBS = queue.Queue()   # MLX runs on the thread that loaded the model, so one thread does all synthesis
READY = threading.Event()


def gpu_thread():
    from mlx_audio.tts.utils import load_model
    model = load_model("mlx-community/orpheus-3b-0.1-ft-4bit")
    READY.set()
    while True:
        text, voice, fut = JOBS.get()
        try:
            chunks, rate = [], 24000
            for r in model.generate(text=text, voice=voice, verbose=False):
                chunks.append(np.array(r.audio))
                rate = getattr(r, "sample_rate", rate)
            buf = io.BytesIO()
            sf.write(buf, np.concatenate(chunks) if chunks else np.zeros(1), rate, format="WAV", subtype="PCM_16")
            fut.set_result(buf.getvalue())
        except Exception as exc:
            fut.set_exception(exc)


def synth(text, voice="leo"):
    fut = Future()
    JOBS.put((text, voice, fut))
    return fut.result(timeout=300)


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
            if not READY.is_set():
                return self._send(503, b'{"ok": false, "loading": true}', "application/json")
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
            self._send(200, synth(text, req.get("voice") or "leo"), "audio/wav")
        except Exception as exc:
            self._send(500, json.dumps({"error": str(exc)}).encode(), "application/json")


if __name__ == "__main__":
    threading.Thread(target=gpu_thread, daemon=True).start()
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
