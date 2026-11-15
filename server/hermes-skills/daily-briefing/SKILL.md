---
name: daily-briefing
description: "Morning briefing: weather, calendar, reminders, topics."
version: 1.0.0
author: the Timmy contributors
license: Apache-2.0
platforms: [macos]
metadata:
  hermes:
    tags: [briefing, morning, weather, calendar, reminders, schedule]
    category: productivity
    requires_toolsets: [terminal]
---

# Daily briefing

A short morning summary: local weather (set TIMMY_PLACE, TIMMY_LAT, TIMMY_LON), today's Apple Calendar events, Reminders due today or overdue, and the next step for each active topic. It runs on a schedule (weekdays 7:30) and on request ("brief me", "what's my day look like?").

Scripts (run with the terminal tool, using Hermes' own Python):

```
HPY=$(ls -td ~/.hermes/installs/*/environments/*/venv | head -1)/bin/python
S="$TIMMY_REPO/server/hermes-skills"
"$HPY" "$S/daily-briefing/scripts/briefing_data.py"                   # facts as JSON
printf '%s' "<briefing text>" | "$HPY" "$S/daily-briefing/scripts/publish_briefing.py" --speak
```

## When to Use

- The scheduled "Timmy daily briefing" cron job.
- "Brief me", "morning briefing", "what's my day?", "what's on today?"

## Procedure

1. Run `briefing_data.py`. It prints JSON with `weather`, `calendar`, `reminders` and `topics`. If a part has an `"error"`, skip it in the briefing and mention it in one short line at the end (e.g. "I couldn't read your calendar: it needs permission").
2. Write the briefing as spoken English, about 60–120 words, no markdown, no bullet symbols, no emoji:
   - Greeting with the day ("Good morning, it's Monday, October 26.")
   - Weather: now, high/low and rain chance in one sentence ("50 and overcast now, high of 55, 45% chance of drizzle").
   - Calendar: count plus the first two or three events with times. If none, say the calendar is clear.
   - Reminders: overdue first, then due today. Skip if none.
   - Topics: one short clause per active topic from `topics[].next`, shortened to plain words. At most four.
   - Close with one short line.
3. Publish: pipe the briefing text into `publish_briefing.py --speak`. It saves it for the web page and reads it aloud on the Mac. When the user asks in a chat (terminal or web page), don't pass `--speak`, since the reply is already spoken there, but still publish it.
4. Your final answer is the briefing text itself.

## Pitfalls
- Read-only: never create, change or delete calendar events or reminders from this skill.
- Calendar and Reminders need macOS permission the first time (Automation for Calendar and Reminders, plus their own privacy access). If osascript errors with -1743 or "not allowed", say so once, in the closing line.
- Weather comes from Open-Meteo (free, no key). Don't use paid weather APIs.
- No work data: if an event clearly comes from a work calendar, just say "work meetings" with the count, no details.

## Verification
- `~/.timmy/briefings/<today>.md` exists and matches what you said.
