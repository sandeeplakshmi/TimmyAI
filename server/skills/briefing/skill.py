# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Morning briefing for the board.

If a briefing for today was already written elsewhere (briefing.reuse_dir, e.g. a web app's daily
briefing folder with <YYYY-MM-DD>.md files), that text is used, so the two never disagree. Otherwise it's
built here: weather (the weather skill), today's reminders (the reminders skill) and, if
briefing.calendar_ics points to a local .ics file, today's events.
"""

import re
from datetime import datetime, timedelta
from pathlib import Path


def setup(kit):
    cfg = kit.config
    kit.daily(cfg.get("time", "07:30"), "scheduled", days=cfg.get("days") or ["mon", "tue", "wed", "thu", "fri"])


def handle(kit, req):
    kit.eyes("morning", secs=6)
    return build(kit)


def scheduled(kit, data):
    # A briefing written elsewhere at the same time may still be on its way: wait for it a little.
    kit.schedule(0 if _reused(kit) else 240, "deliver", {})


def deliver(kit, data):
    kit.say(build(kit), eyes="morning")


def _reused(kit):
    d = kit.config.get("reuse_dir")
    f = Path(d).expanduser() / f"{kit.now():%Y-%m-%d}.md" if d else None
    if f and f.exists():
        text = re.sub(r"[#*_>`]", "", f.read_text()).strip()
        return re.sub(r"\s+", " ", text)
    return None


def _ics_today(path):
    try:
        text = Path(path).expanduser().read_text()
    except OSError:
        return []
    text = re.sub(r"\r?\n[ \t]", "", text)   # unfold long lines
    today = datetime.now().strftime("%Y%m%d")
    out = []
    for ev in re.findall(r"BEGIN:VEVENT(.*?)END:VEVENT", text, re.S):
        start = re.search(r"^DTSTART[^:]*:(\d{8})(?:T(\d{4}))?", ev, re.M)
        summary = re.search(r"^SUMMARY[^:]*:(.*)$", ev, re.M)
        if start and summary and start.group(1) == today:
            t = start.group(2)
            when = datetime.strptime(t, "%H%M").strftime("%I:%M %p").lstrip("0") if t else "all day"
            out.append((t or "0000", f"{summary.group(1).strip()} {('at ' + when) if t else when}"))
    return [s for _, s in sorted(out)]


def build(kit):
    reused = _reused(kit)
    if reused:
        return reused
    name = kit.config.get("name")
    parts = [f"Good morning{', ' + name if name else ''}. It's {kit.now():%A, %B} {kit.now().day}."]
    w = kit.skill("weather")
    if w:
        try:
            data = w[0].fetch(w[1])
            if data:
                parts.append(w[0].sentence(data))
        except Exception:
            parts.append("I couldn't get the weather.")
    r = kit.skill("reminders")
    if r:
        end = (kit.now() + timedelta(days=1)).replace(hour=0, minute=0, second=0).timestamp()
        today = [j for j in r[1].jobs("fire") if j["at"] < end]
        if today:
            items = [f"{j['label']} at {kit.say_time(datetime.fromtimestamp(j['at']))}" for j in today[:4]]
            parts.append("Reminders today: " + "; ".join(items) + ".")
    ics = kit.config.get("calendar_ics")
    if ics:
        events = _ics_today(ics)
        parts.append(("On your calendar: " + "; ".join(events[:4]) + ".") if events else "Your calendar is clear.")
    return " ".join(parts)
