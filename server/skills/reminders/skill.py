# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Reminders and timers, kept in the skill kit's schedule (survives restarts).

At the time: the eyes glow for a few seconds, then Timmy speaks it. If no board is online it waits for
one; more than reminders.late_quiet_minutes late it's shown on screen instead of spoken.
"""


def handle(kit, req):
    m = req.match
    if "cancel" in m:
        kind = "timer" if m["cancel"].startswith("timer") else "reminder"
        gone = kit.cancel(call="ring" if kind == "timer" else "fire")
        return f"Cancelled {len(gone)} {kind}{'s' * (len(gone) != 1)}." if gone else f"You don't have any {kind}s."
    if "dur" in m:
        secs = kit.parse_duration(m["dur"])
        if not secs:
            return "How long should the timer be?"
        kit.schedule(secs, "ring", {"secs": secs}, label=_timer_name(kit, secs))
        kit.eyes("set", secs=2)
        return f"Timer set for {kit.say_duration(secs)}."
    if "what" in m:
        when = kit.parse_when(m["when"])
        if not when:
            return "When should I remind you?"
        what = m["what"].strip().rstrip(".")
        kit.schedule(when, "fire", {"what": what}, label=what)
        kit.eyes("set", secs=2)
        return f"Okay, I'll remind you {_at(kit, when)} to {what}."
    jobs = kit.jobs()
    if not jobs:
        return "You have no reminders or timers."
    parts = []
    for j in jobs[:5]:
        when = kit.now().fromtimestamp(j["at"])
        parts.append(f"{j['label']} {_at(kit, when)}" if j["call"] == "fire" else f"a {j['label']} ending {_at(kit, when)}")
    return "You have " + "; ".join(parts) + "."


def _at(kit, when):
    t = kit.say_time(when)
    return t if t.startswith(("tomorrow", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")) else "at " + t


def _quiet(kit, data):
    return data.get("_late_s", 0) > 60 * float(kit.config.get("late_quiet_minutes", 30))


def fire(kit, data):
    kit.say(f"Reminder: {data['what']}.", silent=_quiet(kit, data), eyes="nudge")


def ring(kit, data):
    kit.say(f"Your {_timer_name(kit, data['secs'])} is done.", silent=_quiet(kit, data), eyes="nudge")


def _timer_name(kit, secs):
    """'10 minutes' -> '10-minute timer'."""
    d = kit.say_duration(secs)
    return (d.replace("minutes", "minute").replace("seconds", "second").replace("hours", "hour")
            .replace(" ", "-", 1) + " timer") if " " in d and d.count(" ") == 1 else d + " timer"
