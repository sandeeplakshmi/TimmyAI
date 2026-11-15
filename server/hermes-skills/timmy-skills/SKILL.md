---
name: timmy-skills
description: "Timmy's local skills: focus timer, weather, reminders and timers, quick notes, desk-buddy nudges, morning briefing."
version: 1.0.0
author: the Timmy contributors
license: Apache-2.0
platforms: [macos]
metadata:
  hermes:
    tags: [timer, focus, pomodoro, weather, reminder, alarm, note, notes, briefing, nudge, desk gadget]
    category: productivity
    requires_toolsets: [terminal]
---

# Timmy's local skills

Small, fast skills that run inside Timmy's web server and drive the desk gadget (its eyes and voice).
Spoken requests on the gadget and typed ones on the web page usually reach them directly; use this when a
request clearly belongs to one but came to you instead (e.g. phrased unusually), or when you want to
combine one with your own answer.

Call (terminal tool), with the request as a short plain phrase in the skill's own words:

```
curl -s -X POST http://127.0.0.1:8000/skills/run \
     -H 'Content-Type: application/json' -d '{"text": "remind me at 3 to call mom"}'
```

It returns `{"reply": "..."}`: say that reply to the user (it's already worded for speech). `null` means no
skill fit the phrase: rephrase it closer to the examples, or name the skill with `"skill": "<name>"`.
`curl -s http://127.0.0.1:8000/skills` lists the skills with examples.

## Skills and phrases

- **focus**: "focus 25 minutes", "how much focus time is left", "stop focus"
- **weather**: "weather", "weather in Paris", "set my city to Denver"
- **reminders**: "remind me at 3 to call mom", "remind me to stretch in 20 minutes", "timer 10 minutes",
  "what are my reminders", "cancel my reminders" (times are local; "at 3" means 3 PM unless "morning")
- **notes**: "note: buy filters", "read my notes"
- **buddy**: "good night", "wake up", "turn on water reminders", "turn off posture reminders"
- **briefing**: "good morning" (board briefing: weather, today's reminders, optional calendar file)

## Rules

- Never send finance data or account details through skills.
- Settings (city, quiet hours, nudge intervals, briefing time) are in ~/.timmy/skill-state/config.yaml, outside the
  repo; skills/config.example.yaml in the repo documents them.
