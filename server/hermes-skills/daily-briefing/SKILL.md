---
name: daily-briefing
description: "Morning briefing and every calendar question (any day or range: today, tomorrow, friday, this week, a date): weather, calendar, reminders, topics."
version: 1.0.0
author: the Timmy contributors
license: Apache-2.0
platforms: [macos]
metadata:
  hermes:
    tags: [briefing, morning, weather, calendar, events, schedule, agenda, reminders, tomorrow, week]
    category: productivity
    requires_toolsets: [terminal]
---

# Daily briefing

A short morning summary: local weather (set TIMMY_PLACE, TIMMY_LAT, TIMMY_LON), Apple Calendar events, Reminders due (and overdue), and the next step for each active topic. It runs on a schedule (weekdays 7:30) and on request ("brief me", "what's my day look like?"). The same scripts answer **every calendar question** for any day or range.

Scripts (run with the terminal tool, using Hermes' own Python):

```
HPY=$(ls -td ~/.hermes/installs/*/environments/*/venv | head -1)/bin/python
D="${HERMES_SKILL_DIR}/scripts"   # this skill's folder
"$HPY" "$D/calendar_events.py" tomorrow           # calendar only, JSON, under a second
"$HPY" "$D/briefing_data.py"                      # briefing facts for today, JSON
"$HPY" "$D/briefing_data.py" --when tomorrow      # briefing facts for another day
printf '%s' "<briefing text>" | "$HPY" "$D/publish_briefing.py" --speak
```

`calendar_events.py` and `--when` take one day or a range, in plain words: `today`, `tomorrow`, `yesterday`, `friday`, `next monday`, `this week`, `next week`, `this weekend`, `next 3 days`, `this month`, `2026-11-20`, `11/20`, `nov 20`, `"nov 20 to nov 22"`, `"friday through sunday"`. Pass only the date words, not the whole question. Add `--all-calendars` only when the user asks about holidays or birthdays (those calendars are skipped by default).

## When to Use

- The scheduled "Timmy daily briefing" cron job.
- "Brief me", "morning briefing", "what's my day?", "what's on today?"
- Any calendar question: "any events tomorrow?", "am I free Friday afternoon?", "what's on this week?", "when is my dentist appointment?" (search a range such as `next month`). Use `calendar_events.py` for these; skip weather, reminders and topics unless asked.

## Never

- **Never write your own AppleScript, JXA or `osascript` for Calendar**, and never loop over calendars or events with `tell application "Calendar"`. That takes many seconds and floods macOS with permission checks. If `calendar_events.py` can't express the range, call it with the nearest wider range and filter the JSON yourself.

## Procedure

1. Run `briefing_data.py` (add `--when tomorrow` etc. for another day). It prints JSON with `weather`, `calendar`, `reminders` and `topics`. If a part has an `"error"`, skip it in the briefing and mention it in one short line at the end (e.g. "I couldn't read your calendar: it needs permission").
2. Write the briefing as spoken English, about 60–120 words, no markdown, no bullet symbols, no emoji:
   - Greeting with the day ("Good morning, it's Monday, October 26.")
   - Weather: now, high/low and rain chance in one sentence ("50 and overcast now, high of 55, 45% chance of drizzle").
   - Calendar: count plus the first two or three events with times. If none, say the calendar is clear.
   - Reminders: overdue first, then due today. Skip if none.
   - Topics: one short clause per active topic from `topics[].next`, shortened to plain words. At most four.
   - Close with one short line.
3. Publish: pipe the briefing text into `publish_briefing.py --speak`. It saves it for the web page and reads it aloud on the Mac. When the user asks in a chat (terminal or web page), don't pass `--speak`, since the reply is already spoken there, but still publish it.
4. Your final answer is the briefing text itself.

## Calendar answers

`calendar_events.py` prints `{"from", "to", "label", "count", "events": [{"day", "weekday", "time", "end", "title", "calendar", "location"?, "until"?}]}`. `time` is 24-hour local time or `"all day"`; `until` marks a multi-day all-day event. Answer in one or two spoken sentences: the count, then the events in order with times in 12-hour form ("You've got two things tomorrow: dentist at 9:30 and dinner with Sam at 7."). Nothing found: "Your calendar's clear tomorrow." For a range, group by day.

If the JSON has `"permission"`, calendar access isn't granted: say "I can't read your calendar yet; it needs calendar access in System Settings, Privacy and Security, Calendars", once. Don't retry and don't fall back to AppleScript.

## Pitfalls
- Read-only: never create, change or delete calendar events or reminders from this skill.
- Calendar is read through EventKit, which needs **Calendars** access (System Settings → Privacy & Security → Calendars) for the app running Hermes. The scripts never pop up the macOS prompt themselves; `calendar_events.py --request-access` does, once, and only when the user is at the Mac and has said yes. Reminders still need Automation access for Reminders. If osascript errors with -1743 or "not allowed", say so once, in the closing line.
- Weather comes from Open-Meteo (free, no key). Don't use paid weather APIs.
- No work data: if an event clearly comes from a work calendar, just say "work meetings" with the count, no details.

## Verification
- `~/.timmy/briefings/<today>.md` exists and matches what you said.
