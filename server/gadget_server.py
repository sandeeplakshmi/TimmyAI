# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Timmy gadget endpoint: a push-to-talk voice terminal (the Waveshare ESP32-S3 AMOLED board) talks to Timmy.

WebSocket on 0.0.0.0:8790 (TIMMY_GADGET_PORT), path /gadget. Started by timmy_server.serve() in its own thread,
only when at least one device key exists. Nothing here opens a route from outside your network.

Keys: $TIMMY_HOME/keys/gadget-keys.json (default ~/.timmy) (mode 600, never in git): {"<device name>": "<64 hex chars>"}.
Make one with:  python server/gadget_server.py --new-key <device name>   (prints it once, for >timmy.key=...)
                --list (board names), --revoke <device name>

Protocol (text frames are JSON, binary frames are 16 kHz mono int16 little-endian PCM):
  server -> {"type":"hello","nonce":"<32 hex>"}
  client -> {"type":"auth","device":"<name>","mac":"<hex HMAC-SHA256(key, nonce)>"}   (the key never travels)
  server -> {"type":"ready"}                       (or closes with 4001 on a bad mac)
  client -> {"type":"begin"}, binary mic PCM while the button is held, {"type":"end"}
            {"type":"cancel"} drops the current turn (button pressed again during a reply)
            {"type":"ping"} -> {"type":"pong"}
  server -> {"type":"state","state":"listening|thinking|speaking|idle"}
            {"type":"heard","text":"..."}            what Whisper heard
            {"type":"say","text":"<sentence>","frames":N,"mood":"<mood>"} then N frames of reply PCM
                                                     (binary, in pieces); mood is one of MOODS, guessed
                                                     from the sentence, for the board's face
            {"type":"done"}                          reply finished (all audio sent)
            {"type":"error","text":"..."}
            {"type":"eyes","look":"<name>",...}           an eye look from a skill (skills/README.md)
            {"type":"push","silent":bool}             Timmy speaks unasked (a reminder, a nudge): the board
                                                     answers push_ok (then say/frames/done follow, as a
                                                     reply) or push_busy; silent: captions only
            {"type":"fallback","url":"wss://<host>/gadget"}   the fallback server address for when the
                                                     home address fails (from TIMMY_GADGET_FALLBACK_URL,
                                                     $TIMMY_HOME/gadget-fallback-url.txt or an add-on):
                                                     the board saves it
Turns go to the same Hermes session as the web page (web-session.json), so Timmy remembers both.
"""

import hashlib
import hmac
import json
import os
import re
import secrets
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import skillkit  # noqa: E402  (ui/skillkit.py: the skills, shared with the web server)

HERMES_HOME = Path(os.environ.get("HERMES_HOME", Path.home() / ".hermes"))
TIMMY_HOME = Path(os.environ.get("TIMMY_HOME") or Path.home() / ".timmy")
KEYS_FILE = TIMMY_HOME / "keys" / "gadget-keys.json"
LOG_FILE = HERMES_HOME / "logs" / "timmy-gadget.log"
PORT = int(os.environ.get("TIMMY_GADGET_PORT", "8790"))
MIC_RATE = 16000
MAX_TURN_S = 60
PCM_PIECE = 3200   # bytes per binary frame sent to the board: 100 ms
# Fallback server address the boards use when the home address doesn't answer: a fixed one from
# TIMMY_GADGET_FALLBACK_URL or $TIMMY_HOME/gadget-fallback-url.txt, or one an add-on provides
# (FALLBACK_PROVIDERS: functions returning a wss:// URL or None, tried first).
FALLBACK_PROVIDERS = []
FALLBACK_FILE = TIMMY_HOME / "gadget-fallback-url.txt"
# Client-IP headers a reverse proxy in front of this endpoint may add.
PROXY_IP_HEADERS = ("Cf-Connecting-Ip", "X-Forwarded-For", "X-Real-Ip")
AUTH_FAIL_MAX, AUTH_FAIL_WINDOW, AUTH_LOCKOUT = 5, 600, 120   # per client: 5 bad logins in 10 min -> 2 min out

SYSTEM_PROMPT = ("You are Timmy, a personal assistant, talking by voice through a small round "
                 "desk gadget (a push-to-talk speaker with a little screen). Replies are spoken aloud: keep them "
                 "to one to three short sentences, no markdown, no lists, unless the user asks for detail. Be friendly. "
                 "For calendar questions use the daily-briefing skill's calendar_events.py; never script Calendar.")

MOODS = ("happy", "excited", "curious", "sad", "love", "laughing", "surprised", "neutral")
MOOD_TAG = re.compile(r"\[\s*(" + "|".join(MOODS) + r")\s*\]\s*", re.I)


# The board's face shows a mood per sentence. It is read from the reply's own words (guess_mood):
# asking Timmy to tag its replies would leave the tags in the conversation the web page shares.
# A tag Timmy writes anyway is still honoured, and never spoken.
def mood_tagged(deltas, out):
    """Passes the reply's text through without [mood] tags; the first tag goes into out["mood"]."""
    head = ""
    for d in deltas:
        if head is not None:
            head += d
            m = MOOD_TAG.match(head.lstrip())
            if m:
                out.setdefault("mood", m.group(1).lower())
                d, head = head.lstrip()[m.end():], None
            elif len(head) > 24 or ("[" not in head and head.strip()):
                d, head = head, None
            else:
                continue
        d = MOOD_TAG.sub("", d)
        if d:
            yield d
    if head:
        yield MOOD_TAG.sub("", head)


def guess_mood(sentence):
    """A sentence's mood when Timmy gave none: a few plain cues, else neutral."""
    t = sentence.lower()
    if re.search(r"\b(haha|hehe|lol)\b|😂|🤣", t):
        return "laughing"
    if re.search(r"\b(love|adore|sweet)\b|❤", t):
        return "love"
    if re.search(r"\b(sorry|unfortunately|sadly|afraid)\b", t):
        return "sad"
    if re.search(r"\b(wow|whoa|really\?|no way)\b", t):
        return "surprised"
    if re.search(r"\b(awesome|amazing|fantastic|great news|congrat)", t) or t.count("!") >= 2:
        return "excited"
    if t.rstrip().endswith("?"):
        return "curious"
    if "!" in t or re.search(r"\b(glad|great|nice|happy)\b", t):
        return "happy"
    return "neutral"


STATUS_FILE = TIMMY_HOME / "gadget-status.json"   # last connection per board, for doctor and flash.sh


def note_board(device, peer, fw):
    """Record a board's last connection (time, address, firmware version) in STATUS_FILE."""
    try:
        data = json.loads(STATUS_FILE.read_text()) if STATUS_FILE.exists() else {}
    except (OSError, ValueError):
        data = {}
    data[device] = {"last_connected": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "epoch": int(time.time()),
                    "peer": peer, "firmware": fw}
    try:
        STATUS_FILE.parent.mkdir(parents=True, exist_ok=True)
        STATUS_FILE.write_text(json.dumps(data, indent=1))
    except OSError:
        pass


def log(msg):
    line = time.strftime("%Y-%m-%d %H:%M:%S ") + msg
    try:
        LOG_FILE.parent.mkdir(parents=True, exist_ok=True)
        with open(LOG_FILE, "a") as f:
            f.write(line + "\n")
    except OSError:
        pass
    print("timmy-gadget: " + msg, file=sys.stderr, flush=True)


def load_keys():
    try:
        keys = json.loads(KEYS_FILE.read_text())
        return {str(k): bytes.fromhex(v) for k, v in keys.items() if isinstance(v, str) and len(v) >= 64}
    except (OSError, ValueError):
        return {}


def new_key(device):
    keys = {}
    try:
        keys = json.loads(KEYS_FILE.read_text())
    except (OSError, ValueError):
        pass
    keys[device] = secrets.token_hex(32)
    KEYS_FILE.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(KEYS_FILE, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(keys, f, indent=1)
    os.chmod(KEYS_FILE, 0o600)
    return keys[device]


# ---- The pipeline: the same pieces timmy_server.py gives the web page ----

SRV = None   # timmy_server's helpers (tts_engine, engine_wav, web_session, ...), set by whoever starts this


def _srv():
    return SRV


def transcribe(pcm):
    """Hermes' own STT (local faster-whisper, its configured model and Timmy prompt)."""
    from tools.voice_mode import transcribe_recording
    with tempfile.NamedTemporaryFile(prefix="kr-gadget-", suffix=".wav", delete=False) as tmp:
        with wave.open(tmp, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(MIC_RATE)
            w.writeframes(pcm)
    try:
        result = transcribe_recording(tmp.name)
    finally:
        os.unlink(tmp.name)
    return (result.get("transcript") or "").strip()


def resample_16k(pcm, rate):
    import numpy as np
    x = np.frombuffer(pcm, dtype=np.int16).astype(np.float32)
    if rate != MIC_RATE and len(x):
        n = int(len(x) * MIC_RATE / rate)
        x = np.interp(np.linspace(0, len(x) - 1, n), np.arange(len(x)), x)
    return np.clip(x, -32768, 32767).astype(np.int16).tobytes()


def speak(text):
    """Text -> 16 kHz PCM with Timmy's configured voice (Kokoro, else Piper)."""
    s = _srv()
    clean = re.sub(r"[*_#`>]+", "", text).strip()
    if not clean:
        return b""
    engine, voice, speed = s.tts_engine()
    if engine in s.timmy_voice.ENGINES:
        pcm, rate = s.wav_pcm(s.engine_wav(engine, clean, voice, speed))
    else:
        import io
        chunks = list(s.piper_voice().synthesize(clean))
        pcm, rate = s.wav_pcm(s.wav_bytes(chunks))
    return resample_16k(pcm, rate)


def stream_chat(text):
    """Yields reply text deltas from the Hermes API server, in the web page's persistent session."""
    s = _srv()
    body = {"model": "hermes-agent", "stream": True,
            "messages": [{"role": "system", "content": SYSTEM_PROMPT}, {"role": "user", "content": text}]}
    for attempt in (1, 2):
        req = urllib.request.Request(s.GATEWAY + "/v1/chat/completions", data=json.dumps(body).encode(),
                                     method="POST", headers={
            "Content-Type": "application/json",
            "Authorization": "Bearer " + s.read_env_value("API_SERVER_KEY"),
            "X-Hermes-Session-Id": s.web_session(),
        })
        try:
            with urllib.request.urlopen(req, timeout=600) as r:
                for line in r:
                    line = line.decode("utf-8", "replace").strip()
                    if not line.startswith("data:"):
                        continue
                    data = line[5:].strip()
                    if not data or data == "[DONE]":
                        continue
                    try:
                        delta = (json.loads(data).get("choices") or [{}])[0].get("delta", {}).get("content") or ""
                    except ValueError:
                        delta = ""
                    if delta:
                        yield delta
                return
        except urllib.error.HTTPError as e:
            if e.code in (404, 409) and attempt == 1:
                s.new_session()
                continue
            raise


SENTENCE = re.compile(r"^([\s\S]{12,}?[.!?…])(\s+)")


def sentences(deltas):
    """Groups streamed text into speakable sentences, as the web page does."""
    pending = ""
    for d in deltas:
        pending += d
        while True:
            m = SENTENCE.match(pending) or re.match(r"^([\s\S]+?)\n+", pending)
            if not m:
                break
            yield m.group(1).strip()
            pending = pending[m.end():]
    if pending.strip():
        yield pending.strip()


# ---- Away from home: the fallback address, and failed logins ----

_fallback = {"url": None}


def fallback_url():
    """The board link's fallback wss:// address, or None."""
    url = None
    for provider in FALLBACK_PROVIDERS:
        try:
            url = provider()
        except Exception as exc:
            log(f"fallback provider failed: {exc!r}")
        if url:
            break
    if not url:
        url = os.environ.get("TIMMY_GADGET_FALLBACK_URL", "").strip() or None
    if not url:
        try:
            url = FALLBACK_FILE.read_text().strip() or None
        except OSError:
            url = None
    if url and not url.startswith("wss://"):
        url = None
    if url != _fallback["url"]:
        _fallback["url"] = url
        if url:
            log(f"fallback address {url}")
    return url


_conns = set()
_conns_lock = threading.Lock()


def fallback_watch():
    """Tells connected boards when the fallback address changes."""
    last = fallback_url()
    while True:
        time.sleep(30)
        url = fallback_url()
        if url and url != last:
            with _conns_lock:
                conns = list(_conns)
            for c in conns:
                try:
                    c.send({"type": "fallback", "url": url})
                except Exception:
                    pass
        last = url


def _board_push(text, silent):
    with _conns_lock:
        conns = list(_conns)
    return any(c.push(text, silent) for c in conns)


def _board_eyes(desc):
    with _conns_lock:
        conns = list(_conns)
    for c in conns:
        try:
            c.send({"type": "eyes", **desc})
        except Exception:
            pass


def _board_count():
    with _conns_lock:
        return len(_conns)


skillkit.HUB.attach_boards(_board_push, _board_eyes, _board_count)


_fails = {}
_fails_lock = threading.Lock()


def locked_out(client):
    now = time.time()
    with _fails_lock:
        times = [t for t in _fails.get(client, []) if now - t < AUTH_FAIL_WINDOW]
        _fails[client] = times
        return len(times) >= AUTH_FAIL_MAX and now - times[-1] < AUTH_LOCKOUT


def note_failure(client):
    with _fails_lock:
        _fails.setdefault(client, []).append(time.time())


# ---- One board connection ----

class Conn:
    def __init__(self, ws, device):
        self.ws, self.device = ws, device
        self.lock = threading.Lock()
        self.turn = 0            # bumped by begin/cancel: an older reply stops sending
        self.audio = bytearray()
        self.talking = False
        self.replying = False
        self.push_ack = threading.Event()
        self.push_ok = False

    def speak_out(self, turn, lines, t0, tagged=None, label="reply"):
        """Sends sentences as speech (say + PCM), then done. False if a newer turn cut it off."""
        tagged = tagged if tagged is not None else {}
        first, logged_mood = None, False
        for sentence in lines:
            if turn != self.turn:
                log(f"{self.device}: {label} cancelled")
                return False
            audio = speak(sentence)
            if turn != self.turn:
                return False
            if first is None:
                first = time.time()
                log(f"{self.device}: first audio {first - t0:.2f}s after release")
                self.send({"type": "state", "state": "speaking"})
            frames = len(audio) // 2
            mood = tagged.get("mood") or guess_mood(sentence)
            if not logged_mood:
                logged_mood = True
                log(f"{self.device}: mood {mood}{'' if tagged.get('mood') else ' (guessed)'}")
            self.send({"type": "say", "text": sentence, "frames": frames, "mood": mood})
            for i in range(0, len(audio), PCM_PIECE):
                if turn != self.turn:
                    return False
                self.send(bytes(audio[i:i + PCM_PIECE]))
        if turn == self.turn:
            self.send({"type": "done"})
            log(f"{self.device}: {label} done {time.time() - t0:.2f}s")
            return True
        return False

    def push(self, text, silent):
        """Timmy speaks unasked. True once the board took it (it then plays it)."""
        if self.talking or self.replying:
            return False
        self.push_ack.clear()
        self.send({"type": "push", "silent": bool(silent)})
        if not self.push_ack.wait(3) or not self.push_ok:
            return False
        self.turn += 1
        turn = self.turn
        log(f"{self.device}: push{' (quiet)' if silent else ''}: {text!r}")

        def go():
            self.replying = True
            try:
                self.speak_out(turn, sentences(iter([text])), time.time(), label="push")
            except Exception as exc:
                log(f"{self.device}: push failed: {exc!r}")
            finally:
                self.replying = False

        threading.Thread(target=go, name="timmy-gadget-push", daemon=True).start()
        return True

    def send(self, obj):
        with self.lock:
            self.ws.send(json.dumps(obj) if isinstance(obj, dict) else obj)

    def reply(self, turn, pcm):
        t0 = time.time()
        self.replying = True
        try:
            self.send({"type": "state", "state": "thinking"})
            if len(pcm) < MIC_RATE * 2 * 0.3:
                self.send({"type": "error", "text": "TOO SHORT - HOLD TO TALK"})
                return
            heard = transcribe(pcm)
            t_stt = time.time()
            log(f"{self.device}: heard {heard!r} ({len(pcm) / 32000:.1f}s audio, stt {t_stt - t0:.2f}s)")
            if turn != self.turn:
                return
            if not heard:
                self.send({"type": "error", "text": "DIDN'T CATCH THAT"})
                return
            self.send({"type": "heard", "text": heard})
            answer = skillkit.run(heard, "board")   # a local skill, if one fits; else the brain
            if turn != self.turn:
                return
            if answer:
                log(f"{self.device}: answered by a skill")
                self.speak_out(turn, sentences(iter([answer])), t0)
            else:
                tagged = {}
                self.speak_out(turn, sentences(mood_tagged(stream_chat(heard), tagged)), t0, tagged)
        except Exception as exc:
            log(f"{self.device}: turn failed: {exc!r}")
            try:
                self.send({"type": "error", "text": "TIMMY HAD A PROBLEM"})
            except Exception:
                pass
        finally:
            self.replying = False

    def handle(self, msg):
        if isinstance(msg, (bytes, bytearray)):
            if self.talking and len(self.audio) < MAX_TURN_S * MIC_RATE * 2:
                self.audio += msg
            return
        try:
            m = json.loads(msg)
        except ValueError:
            return
        kind = m.get("type")
        if kind == "begin":
            self.turn += 1
            self.audio = bytearray()
            self.talking = True
            self.send({"type": "state", "state": "listening"})
        elif kind == "end" and self.talking:
            self.talking = False
            pcm, turn = bytes(self.audio), self.turn
            threading.Thread(target=self.reply, args=(turn, pcm), name="timmy-gadget-turn", daemon=True).start()
        elif kind == "cancel":
            self.turn += 1
            self.talking = False
        elif kind == "ping":
            self.send({"type": "pong"})
        elif kind in ("push_ok", "push_busy"):
            self.push_ok = kind == "push_ok"
            self.push_ack.set()


def handler(ws):
    peer = ws.remote_address[0] if ws.remote_address else "?"
    headers = ws.request.headers if ws.request is not None else {}
    proxied = next((headers.get(h) for h in PROXY_IP_HEADERS if headers.get(h)), None)
    if proxied:
        peer = f"{proxied.split(',')[0].strip()} (proxied)"
    if ws.request is not None and ws.request.path.split("?")[0] != "/gadget":
        ws.close(4004, "not found")
        return
    if locked_out(peer):
        log(f"locked out {peer}: too many failed logins")
        ws.close(4029, "too many attempts")
        return
    nonce = secrets.token_hex(16)
    try:
        ws.send(json.dumps({"type": "hello", "nonce": nonce}))
        auth = json.loads(ws.recv(timeout=10))
        device = str(auth.get("device") or "")[:64]
        key = load_keys().get(device)
        want = hmac.new(key, nonce.encode(), hashlib.sha256).hexdigest() if key else secrets.token_hex(32)
        if auth.get("type") != "auth" or not hmac.compare_digest(want, str(auth.get("mac") or "")) or not key:
            log(f"rejected connection from {peer} (device {device!r})")
            note_failure(peer)
            ws.close(4001, "unauthorized")
            return
    except Exception:
        return
    log(f"{device}: connected from {peer}")
    note_board(device, peer, str(auth.get("fw") or "")[:32])
    conn = Conn(ws, device)
    conn.send({"type": "ready"})
    url = fallback_url()
    if url:
        conn.send({"type": "fallback", "url": url})
    with _conns_lock:
        _conns.add(conn)
    threading.Thread(target=skillkit.HUB.board_connected, name="timmy-gadget-catchup", daemon=True).start()
    try:
        for msg in ws:
            conn.handle(msg)
    except Exception as exc:
        log(f"{device}: connection error {exc!r}")
    with _conns_lock:
        _conns.discard(conn)
    conn.turn += 1   # stop any reply in progress
    log(f"{device}: disconnected")


def serve():
    """Blocking. Runs only when a device key exists, so a fresh install exposes nothing."""
    if not load_keys():
        log(f"no device keys in {KEYS_FILE}: gadget endpoint off")
        return
    from websockets.sync.server import serve as ws_serve
    waited = False
    while True:   # the port may still be held by a hand-started copy: take over when it goes
        try:
            server = ws_serve(handler, "0.0.0.0", PORT, max_size=1 << 20, ping_interval=20, ping_timeout=45)
            break
        except OSError as exc:
            if not waited:
                log(f"port {PORT} busy ({exc}); retrying every 10 s")
                waited = True
            time.sleep(10)
    log(f"gadget endpoint on ws://0.0.0.0:{PORT}/gadget")
    threading.Thread(target=fallback_watch, name="timmy-gadget-fallback", daemon=True).start()
    skillkit.ensure_loaded()
    with server:
        server.serve_forever()


def start_thread():
    threading.Thread(target=serve, name="timmy-gadget", daemon=True).start()


if __name__ == "__main__":
    if sys.argv[1:2] == ["--new-key"] and len(sys.argv) > 2:
        print(new_key(sys.argv[2]))
        sys.exit(0)
    if sys.argv[1:2] == ["--list"]:
        print("\n".join(sorted(load_keys())) or "(no boards)")
        sys.exit(0)
    if sys.argv[1:2] == ["--revoke"] and len(sys.argv) > 2:
        try:
            keys = json.loads(KEYS_FILE.read_text())
        except (OSError, ValueError):
            keys = {}
        if keys.pop(sys.argv[2], None) is None:
            sys.exit(f"no board named {sys.argv[2]!r}")
        KEYS_FILE.write_text(json.dumps(keys, indent=1))
        os.chmod(KEYS_FILE, 0o600)
        print(f"revoked {sys.argv[2]} (its next connection is refused; restart the gateway to drop it now)")
        sys.exit(0)
    # Standalone (testing): load the web server's helpers as module "timmy_server", then serve.
    here = Path(__file__).resolve().parent
    sys.path.insert(0, str(here))
    import timmy_server
    SRV = timmy_server
    os.chdir(timmy_server.HERMES_SRC)
    serve()
