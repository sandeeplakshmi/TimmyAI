# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Focus sessions: ring countdown on the eyes, a cheer at the end, then a sleepy break."""

import re


def handle(kit, req):
    t = req.text.lower()
    if re.search(r"\b(stop|cancel|end|quit)\b", t):
        if not kit.cancel(id="end") and not kit.cancel(id="break"):
            return "There's no focus session running."
        kit.eyes(None, tag="focus")
        return "Focus session stopped."
    if re.search(r"\bhow\b|\bleft\b", t):
        for j in kit.jobs():
            left = j["at"] - kit.now().timestamp()
            what = "focus" if j["call"] == "done" else "break"
            return f"{kit.say_duration(max(left, 0))} of {what} left."
        return "There's no focus session running."
    secs = kit.parse_duration(req.match.get("dur") or "") or int(float(kit.config.get("minutes", 25)) * 60)
    kit.cancel()
    kit.schedule(secs, "done", {"secs": secs}, id="end", label="focus")
    kit.eyes("focus", secs=0, tag="focus", ring_total=secs, ring_left=secs)
    return f"Focus for {kit.say_duration(secs)}. I'll keep quiet until then."


def done(kit, data):
    brk = int(float(kit.config.get("break_minutes", 5)) * 60)
    kit.eyes(None, tag="focus")
    kit.say(f"Nice work, that's {kit.say_duration(data['secs'])} of focus. Take a {kit.say_duration(brk)} break.",
            eyes="cheer")
    if brk:
        kit.schedule(brk, "break_over", {}, id="break", label="break")
        kit.eyes("break", secs=0, tag="focus", ring_total=brk, ring_left=brk, delay=6)


def break_over(kit, data):
    kit.eyes(None, tag="focus")
    kit.say("Break's over. Ready when you are.", eyes="ready")
