#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Apple Calendar events for any day or range, as JSON, in one fast EventKit query.

  calendar_events.py [WHEN...] [--all-calendars] [--request-access]

WHEN (default "today"), case-insensitive:
  today, tonight, tomorrow, yesterday, day after tomorrow
  monday ... sunday, this friday (the next one, today included), next friday (after today)
  this week (today to Sunday), next week (Monday to Sunday), this weekend / weekend (the coming
  Saturday and Sunday), next weekend (the one after that)
  this month, next month, next 3 days, 5 days
  2026-11-20, 11/20, 11/20/2026, nov 20, november 20 2026, 20 nov
  ranges: "nov 20 to nov 22", "2026-11-20..2026-11-22", "from friday through sunday" (end included)

Reads events through eventkit_events.js (EventKit inside osascript): one request, no Calendar app,
no per-event Apple Events. Holiday, birthday and other subscribed calendars are skipped unless
--all-calendars is given. Read-only. Without --request-access it never shows a macOS prompt.
"""

import calendar
import json
import re
import subprocess
import sys
import time
from datetime import date, datetime, timedelta
from pathlib import Path

HELPER = Path(__file__).with_name("eventkit_events.js")
WEEKDAYS = ["monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"]
MONTHS = {m.lower(): i for i, m in enumerate(calendar.month_name) if m}
MONTHS.update({m.lower(): i for i, m in enumerate(calendar.month_abbr) if m})
MONTHS["sept"] = 9
RANGE_SPLIT = re.compile(r"\s*(?:\.\.|\bthrough\b|\bthru\b|\buntil\b|\btill\b|\bto\b|–|—|\s-\s)\s*")


class WhenError(ValueError):
    pass


def _weekday(name):
    for i, w in enumerate(WEEKDAYS):
        if name == w or (len(name) >= 3 and w.startswith(name)):
            return i
    return None


def _date(text, today):
    """One day from text, or None."""
    t = text
    if t in ("today", "tonight", "now", ""):
        return today
    if t == "tomorrow":
        return today + timedelta(days=1)
    if t in ("day after tomorrow", "the day after tomorrow"):
        return today + timedelta(days=2)
    if t == "yesterday":
        return today - timedelta(days=1)
    m = re.fullmatch(r"(this |next |coming |on )?([a-z]+)", t)
    if m and _weekday(m.group(2)) is not None:
        ahead = (_weekday(m.group(2)) - today.weekday()) % 7
        if m.group(1) == "next " and ahead == 0:
            ahead = 7
        return today + timedelta(days=ahead)
    m = re.fullmatch(r"(\d{4})-(\d{1,2})-(\d{1,2})", t)
    if m:
        return _make(int(m.group(1)), int(m.group(2)), int(m.group(3)), text)
    m = re.fullmatch(r"(\d{1,2})/(\d{1,2})(?:/(\d{2,4}))?", t)
    if m:
        return _guess_year(int(m.group(1)), int(m.group(2)), m.group(3), today, text)
    m = re.fullmatch(r"([a-z]+)\.? (\d{1,2})(?:st|nd|rd|th)?,? ?(\d{4})?", t)
    if m and m.group(1) in MONTHS:
        return _guess_year(MONTHS[m.group(1)], int(m.group(2)), m.group(3), today, text)
    m = re.fullmatch(r"(\d{1,2})(?:st|nd|rd|th)? (?:of )?([a-z]+)\.?,? ?(\d{4})?", t)
    if m and m.group(2) in MONTHS:
        return _guess_year(MONTHS[m.group(2)], int(m.group(1)), m.group(3), today, text)
    return None


def _make(y, mo, d, text):
    try:
        return date(y, mo, d)
    except ValueError:
        raise WhenError(f"not a real date: {text!r}")


def _guess_year(mo, d, year, today, text):
    """A month/day without a year means the next one (a date in the past rolls to next year)."""
    if year:
        y = int(year)
        return _make(y + 2000 if y < 100 else y, mo, d, text)
    day = _make(today.year, mo, d, text)
    return day if day >= today - timedelta(days=1) else _make(today.year + 1, mo, d, text)


def _span(text, today):
    """(first, last) days, last included, for a named span; None if text isn't one."""
    t = text
    if t in ("this week", "the rest of the week", "rest of the week", "week"):
        return today, today + timedelta(days=6 - today.weekday())
    if t == "next week":
        monday = today + timedelta(days=7 - today.weekday())
        return monday, monday + timedelta(days=6)
    if t in ("this weekend", "weekend", "the weekend"):
        if today.weekday() == 6:
            return today, today
        saturday = today + timedelta(days=(5 - today.weekday()) % 7)
        return saturday, saturday + timedelta(days=1)
    if t == "next weekend":   # the one after this weekend
        saturday = today + timedelta(days=6 if today.weekday() == 6 else (5 - today.weekday()) % 7 + 7)
        return saturday, saturday + timedelta(days=1)
    if t == "this month":
        return today, today.replace(day=calendar.monthrange(today.year, today.month)[1])
    if t == "next month":
        y, mo = (today.year + 1, 1) if today.month == 12 else (today.year, today.month + 1)
        return date(y, mo, 1), date(y, mo, calendar.monthrange(y, mo)[1])
    m = re.fullmatch(r"(?:the )?(?:next|coming) (\d{1,3}) days?|(\d{1,3}) days?", t)
    if m:
        n = int(m.group(1) or m.group(2))
        if n < 1:
            raise WhenError("need at least one day")
        return today, today + timedelta(days=n - 1)
    return None


def parse_when(text="today", today=None):
    """Text -> (first_day, last_day), both included. Raises WhenError when it can't tell."""
    today = today or date.today()
    t = re.sub(r"[?!]", "", (text or "today").lower())
    t = re.sub(r"\s+", " ", t).strip().strip(",.")
    t = re.sub(r"^(on|for|from|between|during|in) ", "", t)
    span = _span(t, today)
    if span:
        return span
    parts = [p for p in RANGE_SPLIT.split(t) if p] if not re.fullmatch(r"\d{4}-\d{1,2}-\d{1,2}", t) else [t]
    if len(parts) == 2:
        a = _date(parts[0], today)
        b = _date(parts[1], today) or _date(f"{parts[0].split()[0]} {parts[1]}", today)  # "nov 20 to 22"
        if a is None or b is None:
            raise WhenError(f"don't understand the range {text!r}")
        if b < a and not re.search(r"\d{4}", parts[1]):
            b = _date(parts[1], a) or b   # "friday to monday": the Monday after Friday
        if b < a:
            raise WhenError(f"range ends before it starts: {text!r}")
        return a, b
    d = _date(t.replace(" and ", " "), today)
    if d is None:
        raise WhenError(f"don't understand {text!r}; try today, tomorrow, friday, this week, 2026-11-20 or nov 20 to nov 22")
    return d, d


def _epoch(d):
    return int(time.mktime(datetime(d.year, d.month, d.day).timetuple()))


def label(first, last):
    if first == last:
        return first.strftime("%A, %B %-d")
    return f"{first.strftime('%A, %B %-d')} to {last.strftime('%A, %B %-d')}"


def fetch(first, last, all_calendars=False, request_access=False, timeout=30):
    """Events from first through last (included), as returned by the EventKit helper."""
    cmd = ["osascript", "-l", "JavaScript", str(HELPER), str(_epoch(first)), str(_epoch(last + timedelta(days=1)))]
    if all_calendars:
        cmd.append("--all-calendars")
    if request_access:
        cmd.append("--request")
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if r.returncode:
        return {"error": f"calendar helper failed: {r.stderr.strip() or r.returncode}"}
    try:
        return json.loads(r.stdout)
    except ValueError:
        return {"error": f"calendar helper gave bad output: {r.stdout[:200]!r}"}


def shape(raw, first, last):
    """Helper output -> the JSON Timmy reads: day, time, end, title, calendar, location."""
    out = {"from": first.isoformat(), "to": last.isoformat(), "label": label(first, last)}
    if raw.get("error"):
        out["error"] = raw["error"]
        if raw.get("status") in ("not_determined", "denied", "restricted", "write_only"):
            out["permission"] = raw["status"]
        return out
    events = []
    for e in raw.get("events", []):
        start, end = datetime.fromisoformat(e["start"]), datetime.fromisoformat(e["end"])
        item = {"day": start.date().isoformat(), "weekday": start.strftime("%a"),
                "time": "all day" if e.get("all_day") else start.strftime("%H:%M"),
                "end": "" if e.get("all_day") else end.strftime("%H:%M"),
                "title": e.get("title") or "(no title)", "calendar": e.get("calendar", "")}
        if e.get("all_day") and (end.date() - start.date()).days > 1:
            item["until"] = (end.date() - timedelta(days=1)).isoformat()
            if start.date() < first:
                item["day"] = first.isoformat()   # started earlier, still on
        if e.get("location"):
            item["location"] = e["location"]
        events.append(item)
    out["count"] = len(events)
    out["events"] = events
    if raw.get("skipped_calendars"):
        out["skipped_calendars"] = raw["skipped_calendars"]
    return out


def events_for(when="today", all_calendars=False, request_access=False, today=None):
    try:
        first, last = parse_when(when, today)
    except WhenError as e:
        return {"error": str(e)}
    if (last - first).days > 92:
        return {"error": "range too long: at most about three months"}
    return shape(fetch(first, last, all_calendars, request_access), first, last)


def main(argv):
    flags = {a for a in argv if a.startswith("--")}
    unknown = flags - {"--all-calendars", "--request-access", "--help"}
    if "--help" in flags or unknown:
        print(__doc__.strip())
        return 0 if not unknown else 2
    when = " ".join(a for a in argv if not a.startswith("--")) or "today"
    t0 = time.monotonic()
    out = events_for(when, "--all-calendars" in flags, "--request-access" in flags)
    out["seconds"] = round(time.monotonic() - t0, 2)
    print(json.dumps(out, indent=1))
    return 1 if "error" in out else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
