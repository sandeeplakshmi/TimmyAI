#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Collect the facts for Timmy's morning briefing and print them as JSON.

  briefing_data.py [--no-calendar] [--no-reminders]

Sources (all free/local): Open-Meteo weather (no key), Apple Calendar and Reminders via osascript,
and the first "Next steps" item of each active topic in $TIMMY_HOME/topics/.
"""

import json
import os
import re
import subprocess
import sys
import urllib.request
from datetime import date
from pathlib import Path

TIMMY_HOME = Path(os.environ.get("TIMMY_HOME", Path.home() / ".timmy"))
TOPICS = TIMMY_HOME / "topics"
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


def weather():
    if not (LAT and LON):
        return {"error": "weather location not set (TIMMY_LAT, TIMMY_LON)"}
    url = ("https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&current=temperature_2m,weather_code"
           "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max,weather_code"
           "&temperature_unit=fahrenheit&wind_speed_unit=mph&timezone=auto&forecast_days=1" % (LAT, LON))
    try:
        with urllib.request.urlopen(url, timeout=15) as r:
            j = json.load(r)
        d, c = j["daily"], j["current"]
        return {"place": PLACE, "now_f": round(c["temperature_2m"]), "now": WMO.get(c["weather_code"], "unknown"),
                "high_f": round(d["temperature_2m_max"][0]), "low_f": round(d["temperature_2m_min"][0]),
                "today": WMO.get(d["weather_code"][0], "unknown"), "rain_chance_pct": d["precipitation_probability_max"][0]}
    except Exception as e:
        return {"error": f"weather unavailable: {e}"}


def osa(script, timeout=60):
    r = subprocess.run(["osascript", "-e", script], capture_output=True, text=True, timeout=timeout)
    if r.returncode:
        raise RuntimeError(r.stderr.strip() or "osascript failed")
    return r.stdout.strip()


CAL_SCRIPT = '''
set out to ""
set dayStart to current date
set time of dayStart to 0
set dayEnd to dayStart + 1 * days
tell application "Calendar"
  repeat with c in calendars
    set evs to (every event of c whose start date < dayEnd and end date > dayStart)
    repeat with e in evs
      set out to out & (start date of e as «class isot» as string) & tab & (allday event of e as string) & tab & (summary of e) & linefeed
    end repeat
  end repeat
end tell
return out
'''

REM_SCRIPT = '''
set out to ""
set dayEnd to current date
set time of dayEnd to 86399
tell application "Reminders"
  repeat with r in (every reminder whose completed is false and due date is not missing value and due date < dayEnd)
    set out to out & (due date of r as «class isot» as string) & tab & (name of r) & linefeed
  end repeat
end tell
return out
'''


def calendar():
    try:
        rows = [l.split("\t") for l in osa(CAL_SCRIPT, 90).splitlines() if l.strip()]
        events = sorted({(t, a == "true", s) for t, a, s in rows})
        return [{"time": "all day" if allday else t[11:16], "title": s} for t, allday, s in events]
    except Exception as e:
        return {"error": f"calendar unavailable: {e}"}


def reminders():
    try:
        rows = [l.split("\t", 1) for l in osa(REM_SCRIPT, 90).splitlines() if l.strip()]
        today = date.today().isoformat()
        return [{"title": s, "overdue": t[:10] < today} for t, s in sorted(rows)]
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
    data = {"date": date.today().strftime("%A, %B %-d"), "weather": weather()}
    if "--no-calendar" not in argv:
        data["calendar"] = calendar()
    if "--no-reminders" not in argv:
        data["reminders"] = reminders()
    data["topics"] = topic_next_steps()
    print(json.dumps(data, indent=1))


if __name__ == "__main__":
    main(sys.argv[1:])
