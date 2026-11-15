# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Timmy's skill kit: small local skills that answer fast, drive the desk gadget's eyes, and run on a schedule.

A skill is a folder skills/<name>/ with:
  skill.yaml   name, description, example phrases, patterns (regexes that pick it), optional tool doc
  skill.py     handle(kit, req) -> reply text, plus any functions its schedule calls
  eyes.json    optional named eye looks the skill plays on the board (see skills/README.md)

The gateway loads every skill at start. A spoken (board) or typed (web page) request is checked against
the skills' patterns first; a match is answered locally, anything else goes to the brain (Hermes), which
can also call skills itself (POST /skills/run, described to it by hermes-skills/timmy-skills).

Personal settings live outside the repo, in <state>/config.yaml (default ~/.timmy/skill-state/config.yaml);
skills/config.example.yaml shows every key. Skill state (reminders, notes, ...) lives in <state> too.
"""

import importlib.util
import json
import os
import re
import threading
import time
import traceback
import uuid
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent / "skills"
STATE = Path(os.environ.get("TIMMY_STATE", Path(os.environ.get("TIMMY_HOME", Path.home() / ".timmy")) / "skill-state"))
LATE_S = 30 * 60          # a spoken message more than this late is shown quietly instead
DAYS = ["mon", "tue", "wed", "thu", "fri", "sat", "sun"]


def _log(msg):
    try:
        STATE.mkdir(parents=True, exist_ok=True)
        with open(STATE / "skills.log", "a") as f:
            f.write(time.strftime("%Y-%m-%d %H:%M:%S ") + msg + "\n")
    except OSError:
        pass


# ---------------------------------------------------------------- config

def load_config():
    """The local config, merged over the example's defaults."""
    cfg = {}
    for path in (ROOT / "config.example.yaml", STATE / "config.yaml"):
        try:
            data = yaml.safe_load(path.read_text()) or {}
        except (OSError, yaml.YAMLError):
            continue
        for k, v in data.items():
            if isinstance(v, dict) and isinstance(cfg.get(k), dict):
                cfg[k] = {**cfg[k], **v}
            else:
                cfg[k] = v
    return cfg


def save_config_value(section, key, value):
    """Sets one value in the local config (keeps everything else)."""
    path = STATE / "config.yaml"
    try:
        data = yaml.safe_load(path.read_text()) or {}
    except (OSError, yaml.YAMLError):
        data = {}
    data.setdefault(section, {})[key] = value
    STATE.mkdir(parents=True, exist_ok=True)
    path.write_text(yaml.safe_dump(data, sort_keys=False))


# ---------------------------------------------------------------- times

NUM_WORDS = {w: i for i, w in enumerate(
    "zero one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen "
    "seventeen eighteen nineteen twenty".split())}
NUM_WORDS.update({"thirty": 30, "forty": 40, "forty-five": 45, "fifty": 50, "sixty": 60, "ninety": 90,
                  "a": 1, "an": 1, "half an": 0.5, "a half": 0.5})
UNIT_S = {"s": 1, "sec": 1, "second": 1, "m": 60, "min": 60, "minute": 60, "h": 3600, "hr": 3600, "hour": 3600}


def _num(word):
    word = word.strip().lower()
    if word in NUM_WORDS:
        return NUM_WORDS[word]
    try:
        return float(word)
    except ValueError:
        return None


def parse_duration(text):
    """'25 minutes', 'an hour and a half', '1h 30m', 'ten seconds' -> seconds, or None."""
    t = text.lower().replace("-", " ")
    total, found = 0.0, False
    for m in re.finditer(r"(\d+(?:\.\d+)?|half an|a half|[a-z]+)\s*(seconds?|secs?|minutes?|mins?|hours?|hrs?|[smh])\b", t):
        n = _num(m.group(1))
        unit = m.group(2).rstrip("s") if len(m.group(2)) > 1 else m.group(2)
        if n is None or unit not in UNIT_S:
            continue
        total += n * UNIT_S[unit]
        found = True
    if re.search(r"\band a half\b", t) and found:
        last = re.findall(r"(seconds?|minutes?|hours?)", t)
        if last:
            total += 0.5 * UNIT_S[last[-1].rstrip("s")]
    return int(total) if found and total > 0 else None


def parse_when(text, now=None):
    """'in 10 minutes', 'at 3', 'at 3:30 pm', 'tomorrow at 9am', 'at noon' -> datetime, or None."""
    now = now or datetime.now()
    t = text.lower().strip().replace(".", "")
    m = re.search(r"\bin\s+(.+)", t)
    if m:
        d = parse_duration(m.group(1))
        if d:
            return now + timedelta(seconds=d)
    day = now
    tomorrow = "tomorrow" in t
    if tomorrow:
        day = now + timedelta(days=1)
    if "noon" in t:
        h, mi, ampm = 12, 0, "pm"
    elif "midnight" in t:
        h, mi, ampm = 0, 0, "am"
    else:
        m = re.search(r"\b(?:at\s+)?(\d{1,2})(?::(\d{2}))?\s*(am|pm|a m|p m)?\b", t)
        if not m:
            return None
        h, mi = int(m.group(1)), int(m.group(2) or 0)
        ampm = (m.group(3) or "").replace(" ", "")
        if h > 23 or mi > 59:
            return None
    if ampm == "pm" and h < 12:
        h += 12
    if ampm == "am" and h == 12:
        h = 0
    if not ampm and 1 <= h <= 6 and "morning" not in t:
        h += 12                     # "at 3" is the afternoon unless said otherwise
        ampm = "pm"
    when = day.replace(hour=h, minute=mi, second=0, microsecond=0)
    if not ampm and h <= 12 and not tomorrow:
        # "at 3": the next 3 o'clock, morning or afternoon
        while when <= now:
            when += timedelta(hours=12)
    elif when <= now and not tomorrow:
        when += timedelta(days=1)
    return when


def say_time(dt, now=None):
    now = now or datetime.now()
    clock = dt.strftime("%I:%M %p").lstrip("0").replace(":00 ", " ")
    if dt.date() == now.date():
        return clock
    if dt.date() == (now + timedelta(days=1)).date():
        return "tomorrow at " + clock
    return dt.strftime("%A at ") + clock


def say_duration(s):
    s = int(round(s))
    if s < 60:
        return f"{s} second{'s' * (s != 1)}"
    m = round(s / 60)
    if m < 60:
        return f"{m} minute{'s' * (m != 1)}"
    h, m = divmod(m, 60)
    return f"{h} hour{'s' * (h != 1)}" + (f" {m} minute{'s' * (m != 1)}" if m else "")


def in_hours(span, now=None):
    """Whether now is inside 'HH:MM-HH:MM' (may wrap midnight)."""
    now = now or datetime.now()
    try:
        a, b = [datetime.strptime(x.strip(), "%H:%M").time() for x in span.split("-")]
    except (ValueError, AttributeError):
        return False
    t = now.time()
    return a <= t < b if a <= b else (t >= a or t < b)


# ---------------------------------------------------------------- the hub (boards, schedule, outbox)

class Hub:
    """What skills act through. The gadget server plugs the boards in with attach_boards()."""

    def __init__(self):
        self.lock = threading.RLock()
        self.skills = {}
        self.jobs_file = STATE / "jobs.json"
        self.outbox_file = STATE / "outbox.json"
        self.jobs = self._read(self.jobs_file, [])
        self.outbox = self._read(self.outbox_file, [])
        self.recurring = []       # (skill, call, rule) re-registered at every start
        self.looks = {}           # persistent eye looks by tag, replayed to a board that connects
        self.board_push = None    # (text, silent) -> bool delivered
        self.board_eyes = None    # (desc) -> None
        self.board_count = lambda: 0
        self.started = False

    @staticmethod
    def _read(path, default):
        try:
            return json.loads(path.read_text())
        except (OSError, ValueError):
            return default

    def _write(self, path, data):
        STATE.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(".tmp")
        tmp.write_text(json.dumps(data, indent=1))
        tmp.replace(path)

    # boards ------------------------------------------------------
    def attach_boards(self, push, eyes, count):
        self.board_push, self.board_eyes, self.board_count = push, eyes, count

    def board_connected(self):
        """A board came online: replay its looks and anything it missed."""
        with self.lock:
            looks = list(self.looks.values())
        for desc in looks:
            self._eyes_out(desc)
        self.flush_outbox()

    def _eyes_out(self, desc):
        if not (self.board_eyes and self.board_count()):
            return
        end = desc.get("_ring_end")
        desc = {k: v for k, v in desc.items() if not k.startswith("_") and k != "delay"}
        if end:
            desc["ring_left"] = max(0, round(end - time.time()))
        try:
            self.board_eyes(desc)
        except Exception:
            pass

    def eyes(self, desc, tag=None):
        desc = dict(desc)
        if "ring_left" in desc:   # countdowns: keep the end time, so a board told later sees the right ring
            desc["_ring_end"] = time.time() + float(desc.get("delay", 0)) + float(desc["ring_left"])
        if desc.get("secs", 0) == 0 and tag:
            with self.lock:
                if desc.get("look"):
                    self.looks[tag] = {**desc, "tag": tag}
                else:
                    self.looks.pop(tag, None)
        if tag:
            desc["tag"] = tag
        delay = float(desc.get("delay", 0))
        if delay > 0:
            threading.Timer(delay, self._eyes_out, args=(desc,)).start()
        else:
            self._eyes_out(desc)

    def say(self, text, silent=False, eyes=None):
        """Speaks on the board now if one is online, else keeps it until one is (late ones go quiet)."""
        item = {"text": text, "at": time.time(), "silent": bool(silent), "eyes": eyes}
        if not self._deliver(item):
            with self.lock:
                self.outbox.append(item)
                self.outbox = self.outbox[-20:]
                self._write(self.outbox_file, self.outbox)
            _log(f"kept for later: {text!r}")

    def _deliver(self, item):
        if not (self.board_push and self.board_count()):
            return False
        late = time.time() - item["at"] > LATE_S
        if item.get("eyes") and not late:
            self._eyes_out(item["eyes"])
        try:
            return bool(self.board_push(item["text"], item["silent"] or late))
        except Exception:
            return False

    def flush_outbox(self):
        with self.lock:
            pending, self.outbox = self.outbox, []
        kept = [it for it in pending if not self._deliver(it)]
        with self.lock:
            self.outbox = kept + self.outbox
            self._write(self.outbox_file, self.outbox)

    # schedule ----------------------------------------------------
    def add_job(self, skill, call, when, data=None, job_id=None, label=None):
        job_id = job_id or f"{skill}-{uuid.uuid4().hex[:10]}"
        with self.lock:
            self.jobs = [j for j in self.jobs if j["id"] != job_id]
            self.jobs.append({"id": job_id, "skill": skill, "call": call, "at": when.timestamp(),
                              "data": data or {}, "label": label})
            self._write(self.jobs_file, self.jobs)
        return job_id

    def cancel_jobs(self, skill, pred=lambda j: True):
        with self.lock:
            gone = [j for j in self.jobs if j["skill"] == skill and pred(j)]
            self.jobs = [j for j in self.jobs if j not in gone]
            self._write(self.jobs_file, self.jobs)
        return gone

    def list_jobs(self, skill):
        with self.lock:
            return sorted((j for j in self.jobs if j["skill"] == skill), key=lambda j: j["at"])

    def add_recurring(self, skill, call, rule):
        self.recurring.append({"skill": skill, "call": call, "rule": rule, "next": None})

    @staticmethod
    def _next(rule, after):
        if "every" in rule:
            return after + timedelta(seconds=rule["every"])
        h, m = map(int, rule["at"].split(":"))
        days = rule.get("days") or DAYS
        d = after.replace(hour=h, minute=m, second=0, microsecond=0)
        if d <= after:
            d += timedelta(days=1)
        for _ in range(8):
            if DAYS[d.weekday()] in days:
                return d
            d += timedelta(days=1)
        return None

    def _run(self, skill_name, call, data):
        sk = self.skills.get(skill_name)
        fn = getattr(sk.module, call, None) if sk else None
        if not fn:
            _log(f"{skill_name}.{call}: no such function")
            return
        try:
            fn(sk.kit, data or {})
        except Exception:
            _log(f"{skill_name}.{call} failed:\n{traceback.format_exc()}")

    def tick(self):
        now = datetime.now()
        with self.lock:
            due = [j for j in self.jobs if j["at"] <= now.timestamp()]
            self.jobs = [j for j in self.jobs if j not in due]
            if due:
                self._write(self.jobs_file, self.jobs)
        for j in due:
            self._run(j["skill"], j["call"], dict(j["data"], _late_s=now.timestamp() - j["at"]))
        for r in self.recurring:
            if r["next"] is None:
                r["next"] = self._next(r["rule"], now)
            elif r["next"] and now >= r["next"]:
                r["next"] = self._next(r["rule"], now)
                self._run(r["skill"], r["call"], {})

    def start(self):
        if self.started:
            return
        self.started = True

        def loop():
            while True:
                try:
                    self.tick()
                except Exception:
                    _log("tick failed:\n" + traceback.format_exc())
                time.sleep(5)

        threading.Thread(target=loop, name="timmy-skills", daemon=True).start()


HUB = Hub()


# ---------------------------------------------------------------- the API a skill sees

class Kit:
    """Passed to a skill's functions. See skills/README.md."""

    def __init__(self, skill):
        self._skill = skill
        self.name = skill.name
        self.dir = skill.path
        self.data_dir = STATE / skill.name

    @property
    def config(self):
        """This skill's section of the local config (plus 'general')."""
        cfg = load_config()
        return {**(cfg.get("general") or {}), **(cfg.get(self.name) or {})}

    def set_config(self, key, value):
        save_config_value(self.name, key, value)

    def say(self, text, silent=False, eyes=None):
        """Speak text on the board (kept until it's online; quiet if it's over 30 min late)."""
        HUB.say(text, silent=silent, eyes=self.look(eyes) if isinstance(eyes, str) else eyes)

    def look(self, name, **extra):
        """An eye look from this skill's eyes.json by name (or a dict), with extra fields."""
        if isinstance(name, dict):
            return {**name, **extra}
        looks = self._skill.looks
        return {**(looks.get(name) or {"look": name}), **extra}

    def eyes(self, look=None, secs=8, tag=None, **extra):
        """Show an eye look on the board for secs (0: until replaced or cleared with eyes(None, tag=...))."""
        desc = self.look(look, secs=secs, **extra) if look else {"look": None, "secs": 0}
        HUB.eyes(desc, tag=tag and f"{self.name}:{tag}")

    def schedule(self, when, call, data=None, id=None, label=None):
        """Run this skill's function `call(kit, data)` at datetime `when` (kept across restarts)."""
        if isinstance(when, (int, float)):
            when = datetime.now() + timedelta(seconds=when)
        return HUB.add_job(self.name, call, when, data, id and f"{self.name}:{id}", label)

    def cancel(self, id=None, call=None):
        return HUB.cancel_jobs(self.name, lambda j: (id is None or j["id"] == f"{self.name}:{id}")
                               and (call is None or j["call"] == call))

    def jobs(self, call=None):
        return [j for j in HUB.list_jobs(self.name) if call is None or j["call"] == call]

    def daily(self, at, call, days=None):
        """Call `call(kit, {})` every day (or on days like ['mon', 'tue']) at 'HH:MM'."""
        HUB.add_recurring(self.name, call, {"at": at, "days": days})

    def every(self, minutes, call):
        HUB.add_recurring(self.name, call, {"every": int(minutes * 60)})

    def boards_online(self):
        return HUB.board_count()

    def skill(self, name):
        """Another skill's module, to reuse its functions (e.g. weather from the briefing)."""
        sk = HUB.skills.get(name)
        return sk and (sk.module, sk.kit)

    now = staticmethod(datetime.now)
    parse_when = staticmethod(parse_when)
    parse_duration = staticmethod(parse_duration)
    say_time = staticmethod(say_time)
    say_duration = staticmethod(say_duration)
    in_hours = staticmethod(in_hours)

    def log(self, msg):
        _log(f"{self.name}: {msg}")


@dataclass
class Request:
    text: str                 # what was said or typed, cleaned up
    match: dict = field(default_factory=dict)   # the pattern's named groups
    source: str = "board"     # board, web or brain


@dataclass
class Skill:
    name: str
    path: Path
    meta: dict
    module: object
    patterns: list
    looks: dict
    kit: object = None


def load_skills():
    for d in sorted(p for p in ROOT.iterdir() if (p / "skill.yaml").exists()):
        try:
            meta = yaml.safe_load((d / "skill.yaml").read_text()) or {}
            spec = importlib.util.spec_from_file_location(f"timmy_skill_{d.name}", d / "skill.py")
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            looks = {}
            if (d / "eyes.json").exists():
                looks = json.loads((d / "eyes.json").read_text())
            sk = Skill(meta.get("name", d.name), d, meta, mod,
                       [re.compile(p, re.I) for p in meta.get("patterns") or []], looks)
            sk.kit = Kit(sk)
            HUB.skills[sk.name] = sk
        except Exception:
            _log(f"skill {d.name} not loaded:\n{traceback.format_exc()}")
    for sk in HUB.skills.values():
        setup = getattr(sk.module, "setup", None)
        if setup:
            try:
                setup(sk.kit)
            except Exception:
                _log(f"{sk.name}.setup failed:\n{traceback.format_exc()}")
    _log("loaded: " + ", ".join(HUB.skills))


_loaded = threading.Lock()


def ensure_loaded():
    """Loads the skills and starts the scheduler, once per process."""
    with _loaded:
        if not HUB.skills:
            load_skills()
            HUB.start()


WAKE = re.compile(r"^\s*(?:hey|hi|ok|okay)?[\s,]*timmy[\s,.!?:]*", re.I)


def clean(text):
    t = WAKE.sub("", text or "").strip()
    return re.sub(r"[\s.!?]+$", "", t).strip()


def find(text):
    """(skill, match dict) for the first skill whose pattern fits, else None."""
    t = clean(text)
    for sk in HUB.skills.values():
        for p in sk.patterns:
            m = p.search(t)
            if m:
                return sk, {k: v for k, v in m.groupdict().items() if v is not None}
    return None


def run(text, source="board", skill=None):
    """Answers text with a skill if one fits (or the named one). Returns the reply, or None for the brain."""
    ensure_loaded()
    if skill:
        sk = HUB.skills.get(skill)
        if not sk:
            return f"I don't have a skill called {skill}."
        hit = (sk, {})
        for p in sk.patterns:
            m = p.search(clean(text))
            if m:
                hit = (sk, {k: v for k, v in m.groupdict().items() if v is not None})
                break
    else:
        hit = find(text)
    if not hit:
        return None
    sk, groups = hit
    try:
        reply = sk.module.handle(sk.kit, Request(clean(text), groups, source))
    except Exception:
        _log(f"{sk.name} failed on {text!r}:\n{traceback.format_exc()}")
        reply = "Sorry, that didn't work."
    _log(f"{source}: {text!r} -> {sk.name}: {reply!r}")
    return reply


def describe():
    """The skills, for the brain's tool list and the README."""
    ensure_loaded()
    return [{"name": s.name, "description": s.meta.get("description", ""),
             "examples": s.meta.get("examples") or []} for s in HUB.skills.values()]
