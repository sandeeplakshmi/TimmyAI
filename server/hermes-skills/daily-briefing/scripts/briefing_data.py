#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Collect the facts for Timmy's morning briefing and print them as JSON.

  briefing_data.py [--when WHEN] [--no-calendar] [--no-reminders] [--no-weather] [--all-calendars]

WHEN is any day or range calendar_events.py understands (default today): tomorrow, friday,
this week, 2026-11-20, "nov 20 to nov 22"... Sources (all free/local): Open-Meteo weather
(no key, for a single day up to a week ahead), Apple Calendar through EventKit (calendar_events.py,
one fast query), Reminders via osascript, and the first "Next steps" item of each active topic
in $TIMMY_HOME/topics/.
"""

import json
import os
import re
import subprocess
import sys
import urllib.request
from datetime import date
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import calendar_events  # noqa: E402

TIMMY_HOME = Path(os.environ.get("TIMMY_HOME") or Path.home() / ".timmy")
TOPICS = TIMMY_HOME / "topics"
# Settings from $TIMMY_HOME/.env when they aren't in the environment (cron jobs don't load it).
try:
    for _line in (TIMMY_HOME / ".env").read_text().splitlines():
        if "=" in _line and not _line.lstrip().startswith("#"):
            _k, _v = _line.split("=", 1)
            if _v.strip() and not os.environ.get(_k.strip()):
                os.environ[_k.strip()] = _v.strip().strip('"').strip("'")
except OSError:
    pass
# Where the weather is for: set TIMMY_LAT / TIMMY_LON / TIMMY_PLACE (e.g. in $TIMMY_HOME/.env).
LAT = os.environ.get("TIMMY_LAT", "")
LON = os.environ.get("TIMMY_LON", "")
PLACE = os.environ.get("TIMMY_PLACE", "")

WMO = {0: "clear", 1: "mostly clear", 2: "partly cloudy", 3: "overcast", 45: "fog", 48: "freezing fog",
       51: "light drizzle", 53: "drizzle", 55: "heavy drizzle", 56: "freezing drizzle", 57: "freezing drizzle",
       61: "light rain", 63: "rain", 65: "heavy rain", 66: "freezing rain", 67: "freezing rain",
       71: "light snow", 73: "snow", 75: "heavy snow", 77: "snow grains", 80: "rain showers",
       81: "rain showers", 82: "heavy rain showers", 85: "snow showers", 86: "heavy snow showers",
       95: "thunderstorms", 96: "thunderstorms with hail", 99: "thunderstorms with hail"}


def weather(day=None):
    """Weather now plus the forecast for day (default today; up to 6 days ahead)."""
    day = day or date.today()
    idx = (day - date.today()).days
    if not 0 <= idx <= 6:
        return {"error": "no forecast for that day"}
    if not (LAT and LON):
        return {"error": "weather location not set (TIMMY_LAT, TIMMY_LON)"}
    url = ("https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&current=temperature_2m,weather_code"
           "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max,weather_code"
           "&temperature_unit=fahrenheit&wind_speed_unit=mph&timezone=auto&forecast_days=%d" % (LAT, LON, idx + 1))
    try:
        with urllib.request.urlopen(url, timeout=15) as r:
            j = json.load(r)
        d, c = j["daily"], j["current"]
        out = {"place": PLACE, "day": day.isoformat(), "high_f": round(d["temperature_2m_max"][idx]),
               "low_f": round(d["temperature_2m_min"][idx]), "today": WMO.get(d["weather_code"][idx], "unknown"),
               "rain_chance_pct": d["precipitation_probability_max"][idx]}
        if idx == 0:
            out.update(now_f=round(c["temperature_2m"]), now=WMO.get(c["weather_code"], "unknown"))
        return out
    except Exception as e:
        return {"error": f"weather unavailable: {e}"}


def osa(script, timeout=60):
    r = subprocess.run(["osascript", "-e", script], capture_output=True, text=True, timeout=timeout)
    if r.returncode:
        raise RuntimeError(r.stderr.strip() or "osascript failed")
    return r.stdout.strip()


# Reminders due before the end of the last day (%(end)d days from today); overdue ones only when the
# range starts today (%(start)d = 0), else due from the first day on.
REM_SCRIPT = '''
set out to ""
set dayStart to current date
set time of dayStart to 0
set dayEnd to dayStart + %(end)d * days
set dayStart to dayStart + %(start)d * days
tell application "Reminders"
  if %(start)d = 0 then
    set rs to (every reminder whose completed is false and due date is not missing value and due date < dayEnd)
  else
    set rs to (every reminder whose completed is false and due date is not missing value and due date >= dayStart and due date < dayEnd)
  end if
  repeat with r in rs
    set out to out & (due date of r as «class isot» as string) & tab & (name of r) & linefeed
  end repeat
end tell
return out
'''


def calendar(first, last, all_calendars=False):
    """Events from first through last via EventKit; a list, or {"error": ...}."""
    out = calendar_events.shape(calendar_events.fetch(first, last, all_calendars), first, last)
    return {k: out[k] for k in ("error", "permission") if k in out} if "error" in out else out["events"]


def reminders(first, last):
    today = date.today()
    start = max((first - today).days, 0)
    try:
        script = REM_SCRIPT % {"start": start, "end": (last - today).days + 1}
        rows = [l.split("\t", 1) for l in osa(script, 90).splitlines() if l.strip()]
        return [{"title": s, "due": t[:10], "overdue": t[:10] < today.isoformat()} for t, s in sorted(rows)]
    except Exception as e:
        return {"error": f"reminders unavailable: {e}"}


def topic_next_steps():
    out = []
    for row in (TOPICS / "README.md").read_text().splitlines():
        m = re.match(r"\|\s*(.+?)\s*\|\s*\[.*?\]\((.+?\.md)\)\s*\|\s*(.*?)\s*\|", row)
        if not m:
            continue
        name, file, status = m.groups()
        if re.match(r"\W*(todo|done|parked)\b", status, re.I):
            continue
        text = (TOPICS / file).read_text()
        for section in ("Next steps", "Open questions"):  # fall back to the first open question
            sec = re.search(r"## %s\s*\n(.*?)(\n## |\Z)" % section, text, re.S)
            items = [l[2:].strip() for l in (sec.group(1) if sec else "").splitlines() if l.startswith("- ")]
            items = [i for i in items if i.lower().rstrip(".") not in ("none yet", "none")]
            if items:
                out.append({"topic": name, "status": status.replace("*", ""), "next": items[0]})
                break
    return out


def main(argv):
    when = "today"
    if "--when" in argv:
        i = argv.index("--when")
        when = argv[i + 1] if i + 1 < len(argv) else ""
    try:
        first, last = calendar_events.parse_when(when)
    except calendar_events.WhenError as e:
        print(json.dumps({"error": str(e)}))
        return 2
    if last < date.today():
        print(json.dumps({"error": "the briefing looks ahead; use calendar_events.py for past days"}))
        return 2
    data = {"date": calendar_events.label(first, last)}
    if "--no-weather" not in argv:
        data["weather"] = weather(first) if first == last else {"error": "weather is for a single day"}
    if "--no-calendar" not in argv:
        data["calendar"] = calendar(first, last, "--all-calendars" in argv)
    if "--no-reminders" not in argv:
        data["reminders"] = reminders(first, last)
    data["topics"] = topic_next_steps()
    print(json.dumps(data, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
