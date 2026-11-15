# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Desk-buddy habits: night sleep, a pout after long neglect (the board does that), water/posture nudges."""

import re

NUDGES = {"water": "Time for a sip of water.", "posture": "Quick posture check: shoulders back, sit tall."}


def setup(kit):
    kit.every(1, "check")          # once a minute: night on or off, nudges due
    _send_settings(kit)


def _send_settings(kit):
    # The board pouts on its own when it wakes after this long without anyone touching it.
    kit.eyes("settings", secs=0, tag="settings", pout_after=int(float(kit.config.get("pout_after_hours", 4)) * 3600))


def _night(kit):
    return kit.in_hours(kit.config.get("quiet_hours", "22:00-07:00"))


_state = {"night": None, "last": {}}


def check(kit, data):
    night = _night(kit)
    if night != _state["night"]:
        _state["night"] = night
        if night:
            kit.eyes("asleep", secs=0, tag="night")
        else:
            kit.eyes(None, tag="night")
            _send_settings(kit)
    if night:
        return
    now = kit.now().timestamp()
    for kind, text in NUDGES.items():
        every = float(kit.config.get(f"{kind}_minutes", 0) or 0)
        if every <= 0:
            continue
        last = _state["last"].setdefault(kind, now)
        if now - last >= every * 60 and kit.boards_online():
            _state["last"][kind] = now
            kit.say(text, eyes=kind)


def handle(kit, req):
    t = req.text.lower()
    m = req.match
    if "kind" in m:
        kind = "posture" if m["kind"] in ("posture", "stretch") else "water"
        if m["onoff"] == "on":
            mins = 75 if kind == "water" else 60
            kit.set_config(f"{kind}_minutes", mins)
            _state["last"][kind] = kit.now().timestamp()
            return f"Okay, a {kind} nudge every {mins} minutes, outside quiet hours."
        kit.set_config(f"{kind}_minutes", 0)
        return f"{kind.capitalize()} nudges are off."
    if re.search(r"night|sleep", t):
        kit.eyes("asleep", secs=0, tag="night")
        _state["night"] = True
        return "Good night!"
    kit.eyes(None, tag="night")
    _state["night"] = _night(kit)   # awake until the next quiet-hours change
    kit.eyes("awake", secs=3)
    return "I'm up!"
