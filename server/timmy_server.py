# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Timmy web server: serves the HUD and bridges it to Hermes.

Run with Hermes' own Python (start-timmy.sh does this) so the page uses the same
voice as the terminal:
  GET  /                     static files from ../web
  POST /v1/chat/completions  proxied to the Hermes API server (key added here,
                             so the browser never sees API_SERVER_KEY). All turns go to one
                             persistent Hermes session (id kept in $TIMMY_HOME/ (default ~/.timmy/)web-session.json),
                             so a page refresh or a restart doesn't lose the conversation.
  GET  /chat/history         recent messages of that session, for the page's chat log
  POST /chat/new             start a fresh Hermes session for the page
  POST /skills/run           {"text": "...", "skill": optional} -> {"reply": text or null}: Timmy's local
                             skills (skills/), for the brain's timmy-skills tool; GET /skills lists them
  POST /voice/transcribe     raw audio body -> Hermes STT (local faster-whisper)
  POST /voice/speak          {"text": "..."} -> Hermes TTS (Piper) audio
  POST /voice/speak-lipsync  {"text": "..."} -> {"audio": <base64 wav>, "words", "wtimes", "wdurations"}
                             Piper audio plus estimated word timings (ms) for the 3D avatar's lip-sync
  GET  /voice/status         {"ok": true, "stt": ..., "tts": ..., "voice": ...}
  GET  /voice/voices         Piper voices Timmy can switch to
  POST /voice/set            {"voice": "ryan", "length_scale": 1.2} -> download if needed, save to Hermes
                             config (length_scale > 1 speaks slower; terminal and page both use it)
  GET  /voice/sample?voice=ryan&length_scale=1.2   a short spoken sample (wav) to audition a voice
  GET  /briefing/latest      newest daily briefing {"date": ..., "text": ...} (written by the daily-briefing skill)
  GET  /alerts/latest        newest alert a skill wrote to $TIMMY_HOME/alerts
  POST /wake/feed?s=<id>     16 kHz mono int16 PCM chunk -> {"detected": bool, "text": request after the
                             wake phrase}; sherpa keyword spotter + local Whisper check of short utterances
  GET  /alerts               the ALERTS panel: alerts, recent briefings, and add-on watches/listings
  GET  /cards                skills (hermes-skills/*/SKILL.md) as small cards
                             with match keywords, so the page can show bubbles when Timmy talks about them
  (also starts gadget_server.py: the desk gadget's WebSocket on 0.0.0.0:8790, only if a device key exists)
  GET  /setup/status         first-run setup state (never secrets); POST /setup/save stores the brain choice
  GET  /wake/debug           save the last ~20 s the wake detector heard to $TIMMY_HOME/ (default ~/.timmy/)wake-debug.wav

Listens on 127.0.0.1 only and rejects requests from other origins.
"""

import base64
import io
import json
import re
import wave
import os
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
HERMES_HOME = Path(os.environ.get("HERMES_HOME", Path.home() / ".hermes"))
HERMES_SRC = HERMES_HOME / "hermes-agent"
REPO_DIR = HERE.parent
WEB_DIR = REPO_DIR / "web"
# Personal data (sessions, briefings, allowed hosts, device keys) lives outside the repo.
TIMMY_HOME = Path(os.environ.get("TIMMY_HOME", Path.home() / ".timmy"))
PORT = int(os.environ.get("TIMMY_PORT", "8000"))
GATEWAY = os.environ.get("TIMMY_GATEWAY", "http://127.0.0.1:8642")
ALLOWED_HOSTS = {f"localhost:{PORT}", f"127.0.0.1:{PORT}"}
EXTRA_HOSTS_FILE = TIMMY_HOME / "allowed-hosts.txt"   # extra Host names the page may be reached as (a private hostname), one per line


def extra_hosts():
    try:
        return {l.strip() for l in EXTRA_HOSTS_FILE.read_text().splitlines() if l.strip() and not l.startswith("#")}
    except OSError:
        return set()
MAX_AUDIO_BYTES = 25 * 1024 * 1024

sys.path.insert(0, str(HERMES_SRC))
sys.path.insert(0, str(HERE))
import skillkit  # noqa: E402

# Voice catalog and name matching are shared with the Hermes "voices" skill.
sys.path.insert(0, str(HERE / "hermes-skills" / "voices" / "scripts"))
import timmy_voice  # noqa: E402


def read_env_value(name):
    try:
        for line in (HERMES_HOME / ".env").read_text().splitlines():
            line = line.strip()
            if line.startswith(name + "="):
                return line.split("=", 1)[1].strip().strip('"').strip("'")
    except OSError:
        pass
    return ""


_PIPER = {}


def piper_voice():
    """The Piper voice Hermes is configured with (tts.piper.voice), loaded once."""
    from piper import PiperVoice
    try:
        import yaml
        name = ((yaml.safe_load((HERMES_HOME / "config.yaml").read_text()) or {}).get("tts") or {}) \
            .get("piper", {}).get("voice") or "en_US-lessac-medium"
    except Exception:
        name = "en_US-lessac-medium"
    path = Path(name) if name.endswith(".onnx") else HERMES_HOME / "cache" / "piper-voices" / f"{name}.onnx"
    if str(path) not in _PIPER:
        if not path.exists():
            timmy_voice.ensure_model(name)
        _PIPER[str(path)] = PiperVoice.load(str(path))
    return _PIPER[str(path)]


def piper_length_scale():
    try:
        import yaml
        tts = (yaml.safe_load((HERMES_HOME / "config.yaml").read_text()) or {}).get("tts") or {}
        return float((tts.get("piper") or {}).get("length_scale") or 1.0)
    except Exception:
        return 1.0


def load_piper(voice_id):
    from piper import PiperVoice
    path = HERMES_HOME / "cache" / "piper-voices" / f"{voice_id}.onnx"
    if str(path) not in _PIPER:
        if not path.exists():
            timmy_voice.ensure_model(voice_id)
        _PIPER[str(path)] = PiperVoice.load(str(path))
    return _PIPER[str(path)]


def wav_bytes(chunks):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(chunks[0].sample_rate if chunks else 22050)
        w.writeframes(b"".join(c.audio_int16_bytes for c in chunks))
    return buf.getvalue()


def speak_with_timings(text):
    """Synthesize text sentence by sentence and spread each sentence's duration over its words,
    weighted by phoneme count (this Piper voice has no alignments). Returns wav bytes + word timings."""
    from piper.config import SynthesisConfig
    engine, kvoice, kspeed = tts_engine()
    voice = piper_voice() if engine == "piper" else None
    syn = SynthesisConfig(length_scale=piper_length_scale())
    clean = re.sub(r"[*_#`>]+", "", text)
    sentences = [x for x in re.split(r"(?<=[.!?;:])\s+", clean.strip()) if x]
    pcm, words, wtimes, wdurs, t_ms, rate = [], [], [], [], 0.0, 22050
    for sent in sentences:
        if engine in timmy_voice.ENGINES:
            audio, rate = wav_pcm(engine_wav(engine, sent, kvoice, kspeed))
            phon_words = []
        else:
            chunks = list(voice.synthesize(sent, syn_config=syn))
            if not chunks:
                continue
            rate = chunks[0].sample_rate
            audio = b"".join(c.audio_int16_bytes for c in chunks)
            phon_words = [w for w in "".join(p for c in chunks for p in c.phonemes).split(" ") if w.strip()]
        dur = len(audio) / 2 / rate * 1000
        text_words = re.findall(r"[A-Za-z0-9'’$%.,-]+", sent)
        text_words = [w for w in text_words if re.search(r"[A-Za-z0-9]", w)]
        if text_words:
            if len(phon_words) == len(text_words):
                weights = [max(1, len(re.sub(r"[ˈˌ,.!?;:]", "", w))) for w in phon_words]
            else:
                weights = [max(1, len(w)) for w in text_words]
            speech = dur * 0.88   # leave ~12% for the pauses at the start and end of a sentence
            t, total = t_ms + dur * 0.04, sum(weights)
            for w, wt in zip(text_words, weights):
                d = speech * wt / total
                words.append(w.strip(".,")); wtimes.append(round(t)); wdurs.append(round(d))
                t += d
        pcm.append(audio)
        t_ms += dur
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
        w.writeframes(b"".join(pcm))
    return buf.getvalue(), words, wtimes, wdurs


# "Hey Timmy" as Whisper writes it, plus near misses. Tested: "Hey Tommy", "Hey Jimmy", "My tummy hurts" don't match.
WAKE_RE = re.compile(r"^\W*(?:hey|hi|hay|heh|okay|ok|a)\W+(?:timmy|timmie|timi|timey)\b\W*(.*)$", re.I | re.S)


SKILL_ALIASES = {   # extra words that mean a skill
    "daily-briefing": ["briefing", "brief me", "morning summary"],
    "mac-control": ["volume", "spotify", "do not disturb", "shortcut"],
    "diy-helper": ["cut list", "shopping list", "diy"],
    "dev-helper": ["git status", "uncommitted"],
    "voices": ["voices", "piper", "new voice"],
    "research": ["does it exist", "closest matches"],
}


# Add-ons can add cards (functions returning a list of card dicts) and alert-panel data (functions
# returning {"watches": [...], "items": [...], "listings": [...]}); see server/addons.py.
CARD_PROVIDERS = []
ALERT_PROVIDERS = []


def cards():
    """The brain's skills (hermes-skills/*/SKILL.md, plus $TIMMY_HOME/hermes-skills) as small cards with
    match keywords, then any add-on cards."""
    out = []
    for provider in CARD_PROVIDERS:
        try:
            out += provider() or []
        except Exception as exc:
            print(f"card provider failed: {exc!r}", file=sys.stderr, flush=True)
    for md in sorted(list((REPO_DIR / "server" / "hermes-skills").glob("*/SKILL.md"))
                     + list((TIMMY_HOME / "hermes-skills").glob("*/SKILL.md"))):
        head = md.read_text().split("---")[1] if md.read_text().startswith("---") else ""
        name = re.search(r"^name:\s*(.+)$", head, re.M)
        desc = re.search(r'^description:\s*"?(.*?)"?$', head, re.M)
        if not name:
            continue
        n = name.group(1).strip()
        out.append({"kind": "skill", "title": n.replace("-", " ").title(), "status": "Skill",
                    "detail": desc.group(1) if desc else "", "lines": skill_lines(n),
                    "keys": sorted(set([n.replace("-", " ")] + SKILL_ALIASES.get(n, [])))})
    return out


def skill_lines(name):
    """Live facts for a skill card, read from the skill's own outputs."""
    try:
        if name == "daily-briefing":
            files = sorted((TIMMY_HOME / "briefings").glob("*.md"))
            if not files:
                return ["No briefing yet. Say “brief me”."]
            sentences = re.split(r"(?<=[.!?])\s+", files[-1].read_text().strip())
            return [f"{files[-1].stem}: " + " ".join(sentences[1:3])[:220]]
        if name == "voices":
            _, _, voice = voice_config()
            return [f"Current voice: {voice}", "Try: “switch to amy”, “ryan”, “list voices”"]
    except Exception:
        pass
    return []


def alerts_feed():
    """What the ALERTS panel shows: alerts skills wrote ($TIMMY_HOME/alerts/*.md), the last two weeks of daily
    briefings, and whatever add-ons provide (watches, items, listings)."""
    items, watches, listings = [], [], []
    for provider in ALERT_PROVIDERS:
        try:
            extra = provider() or {}
            watches += extra.get("watches", []); items += extra.get("items", []); listings += extra.get("listings", [])
        except Exception as exc:
            print(f"alert provider failed: {exc!r}", file=sys.stderr, flush=True)
    for f in sorted((TIMMY_HOME / "alerts").glob("*.md"))[-50:]:
        lines = f.read_text().strip().splitlines()
        d = re.sub(r"\D", "", f.stem)
        stamp = f"{d[0:4]}-{d[4:6]}-{d[6:8]} {d[8:10]}:{d[10:12]}" if len(d) >= 12 else f.stem
        items.append({"kind": "alert", "source": "Alert", "time": stamp, "title": (lines or [""])[0][:140],
                      "text": "\n".join(lines[1:]).strip()[:3000]})
    for f in sorted((TIMMY_HOME / "briefings").glob("*.md"))[-14:]:
        text = f.read_text().strip()
        sents = re.split(r"(?<=[.!?])\s+", text)
        try:
            day = time.strftime("%A, %B %-d", time.strptime(f.stem, "%Y-%m-%d"))
        except ValueError:
            day = f.stem
        first = f"{day}: " + (sents[1] if len(sents) > 1 and sents[0].lower().startswith("good morning") else sents[0])
        items.append({"kind": "briefing", "source": "Daily briefing",
                      "time": time.strftime("%Y-%m-%d %H:%M", time.localtime(f.stat().st_mtime)),
                      "title": first[:140], "text": text[:3000]})
    items.sort(key=lambda i: i["time"], reverse=True)
    return {"watches": watches, "items": items[:50], "listings": listings[:8]}


class WakeSession:
    """Per browser tab: sherpa keyword spotter (fast) plus a Whisper check of each short utterance
    (robust for a real voice saying an unusual name). Everything runs on this Mac."""

    def __init__(self, engine):
        self.engine, self.last_used = engine, time.time()
        self.floor, self.speech, self.silent, self.seg = 300.0, False, 0, []

    def utterances(self, frame, rms):
        """Simple energy VAD: yields a finished utterance (int16 array) after ~0.6 s of quiet."""
        import numpy as np
        self.floor = 0.97 * self.floor + 0.03 * rms if rms < self.floor * 2 else self.floor
        loud = rms > max(350.0, self.floor * 2.5)
        if loud:
            self.speech, self.silent = True, 0
        if self.speech:
            self.seg.append(frame)
            if not loud:
                self.silent += 1
            if self.silent >= 6 or len(self.seg) > 100:          # 0.48 s quiet, or 8 s max
                seg, self.seg, self.speech, self.silent = self.seg, [], False, 0
                if 4 <= len(seg) - 6 <= 94:                    # ~0.3–7.5 s of speech
                    yield np.concatenate(seg)


class WakeSessions:
    def __init__(self):
        self.lock = threading.Lock()
        self.sessions = {}   # id -> WakeSession
        self.recent = {}     # id -> last ~20 s of PCM (for GET /wake/debug)

    def feed(self, sid, pcm):
        """Returns (detected, request_text). request_text is what followed "Hey Timmy" in the same breath."""
        import numpy as np
        from tools.wake_word_engines import _SherpaKwsEngine
        with self.lock:
            now = time.time()
            for k in [k for k, v in self.sessions.items() if now - v.last_used > 300]:
                self.sessions.pop(k).engine.close()
                self.recent.pop(k, None)
            if sid not in self.sessions:
                ww = wake_config()
                self.sessions[sid] = WakeSession(_SherpaKwsEngine({
                    "phrase": ww.get("phrase") or "hey timmy", "sensitivity": ww.get("sensitivity", 0.6),
                    "profile_routing": False}))
            sess = self.sessions[sid]
            sess.last_used = now
            self.recent[sid] = (self.recent.get(sid, b"") + pcm)[-16000 * 2 * 20:]
            samples = np.frombuffer(pcm, dtype=np.int16)
            hit, utterances = False, []
            n = sess.engine.frame_length
            for i in range(0, len(samples), n):
                frame = samples[i:i + n]
                # sherpa alone misses many real voices and fires mid-sentence (losing "…what's the weather"),
                # so the Whisper check below decides; the spotter is kept for its stream state only.
                rms = float(np.sqrt(np.mean(frame.astype(np.float32) ** 2))) if len(frame) else 0.0
                utterances.extend(sess.utterances(frame, rms))
        for utt in utterances:                                   # Whisper outside the lock
            text = transcribe_pcm(utt)
            m = WAKE_RE.match(text)
            if m:
                return True, m.group(1).strip()
        return False, ""


_WAKE_MODEL = None


def transcribe_pcm(samples):
    """Fast Whisper pass just for the wake check: base.en, greedy, told the name is Timmy."""
    global _WAKE_MODEL
    import numpy as np
    from faster_whisper import WhisperModel
    if _WAKE_MODEL is None:
        _WAKE_MODEL = WhisperModel("base.en", device="cpu", compute_type="int8")
    audio = samples.astype(np.float32) / 32768.0
    # This prompt spells the name without biasing "Hey Siri" (tested: "Hey Timmy." as prompt made it hear Siri as Timmy)
    segs, _ = _WAKE_MODEL.transcribe(audio, language="en", beam_size=1, vad_filter=True,
                                     initial_prompt="Timmy is my assistant.", condition_on_previous_text=False)
    return " ".join(x.text for x in segs).strip()


WAKE = WakeSessions()


def wake_config():
    try:
        import yaml
        return (yaml.safe_load((HERMES_HOME / "config.yaml").read_text()) or {}).get("wake_word") or {}
    except Exception:
        return {}


SESSION_FILE = TIMMY_HOME / "web-session.json"


def gateway_json(method, path, payload=None):
    req = urllib.request.Request(GATEWAY + path, method=method,
                                 data=json.dumps(payload).encode() if payload is not None else None,
                                 headers={"Content-Type": "application/json",
                                          "Authorization": "Bearer " + read_env_value("API_SERVER_KEY")})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


def new_session():
    j = gateway_json("POST", "/api/sessions", {"title": time.strftime("Timmy web page %Y-%m-%d %H:%M:%S")})
    sid = j.get("id") or (j.get("session") or {}).get("id")
    SESSION_FILE.parent.mkdir(parents=True, exist_ok=True)
    SESSION_FILE.write_text(json.dumps({"session_id": sid}))
    return sid


def web_session():
    try:
        return json.loads(SESSION_FILE.read_text())["session_id"]
    except (OSError, ValueError, KeyError):
        return new_session()


KOKORO_URL = "http://127.0.0.1:8781"
_KOKORO_LOCK = threading.Lock()


def tts_engine():
    """('kokoro', voice, speed) or ('piper', voice_id, length_scale) from Hermes' config."""
    try:
        import yaml
        tts = (yaml.safe_load((HERMES_HOME / "config.yaml").read_text()) or {}).get("tts") or {}
    except Exception:
        tts = {}
    if tts.get("provider") == "kokoro":
        k = (tts.get("providers") or {}).get("kokoro") or {}
        return "kokoro", k.get("voice") or "am_michael", float(k.get("speed") or 1.0)
    if tts.get("provider") == "orpheus":
        o = (tts.get("providers") or {}).get("orpheus") or {}
        return "orpheus", o.get("voice") or "leo", 1.0
    return "piper", (tts.get("piper") or {}).get("voice") or "en_US-lessac-medium", piper_length_scale()


_WORKER_LOCK = threading.Lock()


def engine_wav(engine, text, voice, speed=1.0):
    """Speech from the engine's helper (kokoro_worker.py / orpheus_worker.py), starting it on first use."""
    py, script, port, _ = timmy_voice.ENGINES[engine]
    url = f"http://127.0.0.1:{port}"

    def call():
        req = urllib.request.Request(url + "/speak", method="POST", headers={"Content-Type": "application/json"},
                                     data=json.dumps({"text": text, "voice": voice, "speed": speed}).encode())
        with urllib.request.urlopen(req, timeout=300) as r:
            return r.read()
    try:
        return call()
    except OSError:
        with _WORKER_LOCK:
            try:
                urllib.request.urlopen(url + "/health", timeout=2)
            except OSError:
                log = open(HERMES_HOME / "logs" / f"{engine}.log", "ab")
                # -I and a clean env: the gateway's own Python settings (3.14 packages) must not leak in
                env = {k: v for k, v in os.environ.items() if not k.startswith("PYTHON") and k != "VIRTUAL_ENV"}
                subprocess.Popen([str(py), "-I", str(timmy_voice.SCRIPTS / script)],
                                 stdout=log, stderr=log, stdin=subprocess.DEVNULL, start_new_session=True, env=env)
                for _ in range(120):   # Orpheus takes a few seconds to load
                    time.sleep(0.5)
                    try:
                        urllib.request.urlopen(url + "/health", timeout=2)
                        break
                    except OSError:
                        pass
        return call()


def kokoro_wav(text, voice, speed):
    return engine_wav("kokoro", text, voice, speed)


def wav_pcm(wav):
    with wave.open(io.BytesIO(wav)) as w:
        return w.readframes(w.getnframes()), w.getframerate()


def voice_config():
    try:
        import yaml
        cfg = yaml.safe_load((HERMES_HOME / "config.yaml").read_text()) or {}
    except Exception:
        cfg = {}
    tts = cfg.get("tts") or {}
    prov = tts.get("provider")
    voice = (f"{prov}:" + (((tts.get("providers") or {}).get(prov) or {}).get("voice") or "")
             if prov in ("kokoro", "orpheus") else (tts.get("piper") or {}).get("voice", ""))
    return (cfg.get("stt") or {}).get("provider", "?"), tts.get("provider", "?"), voice


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(WEB_DIR), **kwargs)

    def log_message(self, fmt, *args):
        sys.stderr.write("timmy-web: " + (fmt % args) + "\n")

    # Only this page on this machine may call us (blocks other sites and DNS rebinding).
    def _allowed(self):
        hosts = ALLOWED_HOSTS | extra_hosts()
        if self.headers.get("Host", "") not in hosts:
            return False
        origin = self.headers.get("Origin")
        return origin is None or origin.split("://", 1)[-1] in hosts

    def _send(self, code, body, ctype="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _body(self, limit):
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0 or n > limit:
            return None
        return self.rfile.read(n)

    def do_GET(self):
        if not self._allowed():
            return self._send(403, {"error": "forbidden"})
        if self.path == "/wake/debug":   # save what the wake detector heard most recently
            with WAKE.lock:
                sid, pcm = max(WAKE.recent.items(), key=lambda kv: getattr(WAKE.sessions.get(kv[0]), "last_used", 0),
                               default=(None, b""))
            out = TIMMY_HOME / "wake-debug.wav"
            with wave.open(str(out), "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000); w.writeframes(pcm)
            return self._send(200, {"session": sid, "seconds": len(pcm) / 32000, "file": str(out)})
        if self.path == "/setup/status":
            import setup_store
            return self._send(200, setup_store.status())
        if self.path == "/alerts":
            return self._send(200, alerts_feed())
        if self.path == "/cards":
            return self._send(200, {"cards": cards()})
        if self.path == "/skills":
            return self._send(200, {"skills": skillkit.describe()})
        if self.path == "/chat/history":
            return self._history()
        if self.path == "/voice/status":
            stt, tts, voice = voice_config()
            return self._send(200, {"ok": True, "stt": stt, "tts": tts, "voice": voice,
                                    "length_scale": round(1 / tts_engine()[2], 2) if tts_engine()[0] == "kokoro"
                                                    else piper_length_scale()})
        if self.path.startswith("/voice/sample?"):
            from urllib.parse import parse_qs, urlparse
            from piper.config import SynthesisConfig
            q = parse_qs(urlparse(self.path).query)
            voice_id = timmy_voice.resolve((q.get("voice") or [""])[0])
            if not voice_id:
                return self._send(400, {"error": "unknown voice"})
            ls = min(2.0, max(0.7, float((q.get("length_scale") or ["1.0"])[0])))
            line = "Hi, I'm Timmy. This is how I would sound when I give you your morning briefing."
            if voice_id.startswith(("kokoro:", "orpheus:")):
                engine, v = voice_id.split(":", 1)
                return self._send(200, engine_wav(engine, line, v, round(1 / ls, 2)), "audio/wav")
            chunks = list(load_piper(voice_id).synthesize(line, syn_config=SynthesisConfig(length_scale=ls)))
            return self._send(200, wav_bytes(chunks), "audio/wav")
        if self.path == "/voice/voices":
            return self._send(200, {"voices": [{"name": k, "about": d, "quality": ["natural"], "engine": "kokoro", "id": "kokoro:" + v}
                                               for k, (v, d) in timmy_voice.KOKORO.items()] +
                                              [{"name": k, "about": d, "quality": ["most human"], "engine": "orpheus", "id": "orpheus:" + v}
                                               for k, (v, d) in timmy_voice.ORPHEUS.items()] +
                                              [{"name": k, "about": d, "quality": q, "engine": "piper"}
                                               for k, (_, d, q) in timmy_voice.VOICES.items()]})
        if self.path in ("/briefing/latest", "/alerts/latest"):
            folder = "briefings" if self.path.startswith("/briefing") else "alerts"
            files = sorted((TIMMY_HOME / folder).glob("*.md"))
            if not files:
                return self._send(200, {"date": None, "text": ""})
            return self._send(200, {"date": files[-1].stem, "text": files[-1].read_text().strip()})
        if self.path.split("?")[0].endswith(".py"):
            return self._send(404, {"error": "not found"})
        return super().do_GET()

    def do_POST(self):
        if not self._allowed():
            return self._send(403, {"error": "forbidden"})
        try:
            if self.path == "/v1/chat/completions":
                return self._chat()
            if self.path == "/client-log":   # browser errors (e.g. from the phone) into the server log
                msg = (self._body(16 * 1024) or b"").decode("utf-8", "replace")[:4000]
                self.log_message("client: %s", msg.replace("\n", " | "))
                with open(HERMES_HOME / "logs" / "timmy-client.log", "a") as f:
                    f.write(time.strftime("%Y-%m-%d %H:%M:%S ") + self.headers.get("User-Agent", "")[:120] + "\n  " + msg + "\n")
                return self._send(200, {"ok": True})
            if self.path == "/skills/run":
                body = json.loads(self._body(64 * 1024) or b"{}")
                reply = skillkit.run(str(body.get("text") or ""), "brain", body.get("skill") or None)
                return self._send(200, {"reply": reply})
            if self.path == "/chat/new":
                return self._send(200, {"ok": True, "session_id": new_session()})
            if self.path == "/voice/transcribe":
                return self._transcribe()
            if self.path == "/voice/speak":
                return self._speak()
            if self.path == "/voice/speak-lipsync":
                text = (json.loads(self._body(64 * 1024) or b"{}").get("text") or "").strip()
                if not text:
                    return self._send(400, {"error": "no text"})
                wav, words, wtimes, wdurs = speak_with_timings(text)
                return self._send(200, {"audio": base64.b64encode(wav).decode(), "words": words,
                                        "wtimes": wtimes, "wdurations": wdurs})
            if self.path == "/voice/set":
                return self._set_voice()
            if self.path == "/setup/save":   # first-run setup page; secrets are stored, never echoed back
                import setup_store
                return self._send(200, setup_store.save(json.loads(self._body(16 * 1024) or b"{}")))
            if self.path.startswith("/wake/feed?s="):
                sid = self.path.split("=", 1)[1][:64]
                pcm = self._body(256 * 1024)
                if not sid or pcm is None:
                    return self._send(400, {"error": "bad wake chunk"})
                detected, text = WAKE.feed(sid, pcm)
                return self._send(200, {"detected": detected, "text": text})
            return self._send(404, {"error": "not found"})
        except Exception as exc:  # keep the page alive, log the cause here
            self.log_message("error on %s: %s", self.path, exc)
            return self._send(500, {"error": "internal error"})

    def _chat(self):
        body = self._body(2 * 1024 * 1024)
        if body is None:
            return self._send(400, {"error": "empty request"})
        # The session holds the history: send only the page's system prompt and the newest user turn.
        req_json = json.loads(body)
        msgs = req_json.get("messages") or []
        req_json["messages"] = [m for m in msgs[:1] if m.get("role") == "system"] + \
                               [m for m in msgs if m.get("role") == "user"][-1:]
        # A local skill (focus, weather, reminders, notes, ...) answers at once, without the brain.
        last = (req_json["messages"][-1].get("content") if req_json["messages"] else "") or ""
        if isinstance(last, list):
            last = " ".join(c.get("text", "") for c in last if isinstance(c, dict))
        reply = skillkit.run(last, "web") if isinstance(last, str) and len(last) < 300 else None
        if reply:
            return self._skill_reply(reply, bool(req_json.get("stream")))
        for attempt in (1, 2):
            sid = web_session()
            req = urllib.request.Request(GATEWAY + "/v1/chat/completions", data=json.dumps(req_json).encode(),
                                         method="POST", headers={
                "Content-Type": "application/json",
                "Authorization": "Bearer " + read_env_value("API_SERVER_KEY"),
                "X-Hermes-Session-Id": sid,
            })
            try:
                with urllib.request.urlopen(req, timeout=600) as r:
                    if not req_json.get("stream"):
                        return self._send(r.status, r.read())
                    # Streamed reply (server-sent events): pass each line through as it arrives, so the page
                    # can start speaking the first sentence while Timmy is still writing the rest.
                    self.send_response(r.status)
                    self.send_header("Content-Type", "text/event-stream")
                    self.send_header("Cache-Control", "no-store")
                    self.end_headers()
                    for line in r:
                        self.wfile.write(line)
                        self.wfile.flush()
                    return
            except urllib.error.HTTPError as e:
                if e.code in (404, 409) and attempt == 1:   # session deleted or not ours: start a new one
                    new_session()
                    continue
                return self._send(e.code, e.read())
            except urllib.error.URLError:
                return self._send(502, {"error": "Hermes gateway is not running (start it with start-timmy.sh)"})

    def _skill_reply(self, text, stream):
        """A skill's answer in the shape of a chat completion, streamed or not."""
        if not stream:
            return self._send(200, {"choices": [{"index": 0, "finish_reason": "stop",
                                                 "message": {"role": "assistant", "content": text}}]})
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        chunk = {"choices": [{"index": 0, "delta": {"role": "assistant", "content": text}}]}
        self.wfile.write(f"data: {json.dumps(chunk)}\n\n".encode())
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()

    def _history(self):
        try:
            sid = web_session()
            j = gateway_json("GET", f"/api/sessions/{sid}/messages?inline_images=false")
        except Exception:
            return self._send(200, {"messages": []})
        rows = j.get("messages") or j.get("data") or []
        out = []
        for m in rows:
            content = m.get("content")
            if isinstance(content, list):
                content = " ".join(c.get("text", "") for c in content if isinstance(c, dict))
            if m.get("role") in ("user", "assistant") and isinstance(content, str) and content.strip():
                out.append({"role": m["role"], "content": content.strip()})
        return self._send(200, {"session_id": sid, "messages": out[-30:]})

    def _transcribe(self):
        audio = self._body(MAX_AUDIO_BYTES)
        if audio is None:
            return self._send(400, {"error": "no audio"})
        ext = ".webm" if "webm" in self.headers.get("Content-Type", "webm") else ".m4a" \
            if "mp4" in self.headers.get("Content-Type", "") else ".wav"
        from tools.voice_mode import transcribe_recording
        with tempfile.NamedTemporaryFile(prefix="kr-web-", suffix=ext, delete=False) as tmp:
            tmp.write(audio)
        try:
            result = transcribe_recording(tmp.name)
        finally:
            os.unlink(tmp.name)
        if not result.get("success") and "empty transcript" not in str(result.get("error", "")).lower():
            return self._send(400, {"error": result.get("error") or "transcription failed"})
        return self._send(200, {"ok": True, "transcript": (result.get("transcript") or "").strip()})

    def _speak(self):
        body = self._body(64 * 1024)
        text = (json.loads(body or b"{}").get("text") or "").strip()
        if not text:
            return self._send(400, {"error": "no text"})
        engine, kvoice, kspeed = tts_engine()
        if engine in timmy_voice.ENGINES:
            return self._send(200, engine_wav(engine, re.sub(r"[*_#`>]+", "", text), kvoice, kspeed), "audio/wav")
        from tools.tts_tool import text_to_speech_tool
        result = text_to_speech_tool(text)
        result = json.loads(result) if isinstance(result, str) else result
        path = result.get("file_path")
        if not result.get("success") or not path or not os.path.isfile(path):
            return self._send(500, {"error": result.get("error") or "speech failed"})
        try:
            audio = Path(path).read_bytes()
        finally:
            os.unlink(path)
        ctype = {".wav": "audio/wav", ".ogg": "audio/ogg", ".mp3": "audio/mpeg"}.get(Path(path).suffix, "audio/mpeg")
        return self._send(200, audio, ctype)

    def _set_voice(self):
        body = self._body(4096)
        want = (json.loads(body or b"{}").get("voice") or "").strip()
        voice_id = timmy_voice.resolve(want)
        if not voice_id:
            return self._send(400, {"error": "unknown voice", "voices": list(timmy_voice.VOICES)})
        timmy_voice.ensure_model(voice_id)
        ls = (json.loads(body or b"{}") or {}).get("length_scale")
        ls = round(min(2.0, max(0.7, float(ls))), 2) if ls is not None else None
        timmy_voice.set_voice(voice_id, ls)
        return self._send(200, {"ok": True, "voice": voice_id, "length_scale": ls})


def start_gadget():
    """The desk gadget's voice endpoint (gadget_server.py), in a thread; off unless a device key exists."""
    try:
        import importlib.util
        import types
        spec = importlib.util.spec_from_file_location("gadget_server", HERE / "gadget_server.py")
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        mod.SRV = types.SimpleNamespace(**globals())
        mod.start_thread()
        return mod
    except Exception as exc:  # the web page must keep working without it
        print(f"Timmy gadget endpoint not started: {exc!r}", file=sys.stderr, flush=True)
        return None


def serve():
    """Run the server (blocking). Also used in-process by the Hermes gateway hook in server/autostart/hook/."""
    print(f"Timmy web: http://localhost:{PORT}  (gateway {GATEWAY})", flush=True)
    skillkit.ensure_loaded()   # skills and their schedule run whether or not a board is connected
    gadget = start_gadget()
    import addons
    addons.load(sys.modules[__name__], gadget)   # private add-ons in $TIMMY_HOME/addons, if any
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()


if __name__ == "__main__":
    os.chdir(HERMES_SRC)  # Hermes tools resolve some paths relative to their checkout
    serve()
