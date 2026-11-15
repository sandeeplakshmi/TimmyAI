# Timmy skills

Small local skills that answer instantly, play looks on the desk gadget's eyes, and run on a schedule.
A request (spoken on the gadget, typed on the web page) is checked against every skill's patterns first;
a match is answered by the skill, anything else goes to the brain. The brain can call skills too
(`POST /skills/run`, see below).

| Skill | Try saying |
|---|---|
| `focus` | "focus 25 minutes", "how much focus time is left", "stop focus" |
| `weather` | "what's the weather", "weather in Chicago", "set my city to Denver" |
| `reminders` | "remind me at 3 to call mom", "timer 10 minutes", "what are my reminders", "cancel my timer" |
| `notes` | "note: buy filters", "read my notes" |
| `buddy` | "good night", "wake up", "turn on water reminders" (plus quiet hours, pouting, nudges on their own) |
| `briefing` | "good morning" (and on a schedule) |
| `hello` | "run the hello skill" (the sample to copy) |

## Settings

Personal settings stay out of the repo, in `config.yaml` in the state folder (`$TIMMY_STATE`, default
`~/.timmy/skill-state/`). `config.example.yaml` here lists every key with its default; copy only what you change.
Some voice commands write the file for you ("set my city to Denver", "turn on water reminders").
The state folder also holds `jobs.json` (reminders and timers), `outbox.json` (things to say once a board
is back), `notes.md` and `skills.log` (what each request matched, and errors).

## Writing a skill

Make a folder `skills/<name>/` with two files (three with eye looks). Start by copying `hello/`.

### skill.yaml

```yaml
name: hello
description: One line for people and for the brain.
examples:                      # phrases that work; shown to the brain
  - "run the hello skill"
patterns:                      # regexes, case-insensitive, searched in what was said ("Hey Timmy" removed)
  - '\bhello skill\b(?: with (?P<extra>\w+))?'     # named groups arrive in req.match
tool: 'How the brain should phrase a call, if not obvious.'
```

Keep patterns specific: they run before the brain, so a loose one steals ordinary questions.

### skill.py

```python
def handle(kit, req):          # required: the reply (said on the board / shown on the page), or None
    ...

def setup(kit):                # optional: runs once at start, e.g. to register a daily schedule
    kit.daily("07:30", "morning", days=["mon", "tue", "wed", "thu", "fri"])

def morning(kit, data):        # any function a schedule names
    kit.say("Good morning!")
```

`req.text` is what was said, `req.match` the pattern's named groups, `req.source` is `board`, `web` or
`brain`.

### The kit

| Call | Does |
|---|---|
| `kit.say(text, silent=False, eyes=None)` | Speak on the board now; kept until a board is online. More than 30 minutes late it's shown, not spoken. `eyes` plays a look first. |
| `kit.eyes(look, secs=8, tag=None, **fields)` | Show a look (a name from `eyes.json`, or any built-in look) for `secs`; `secs=0` keeps it until replaced or cleared with `kit.eyes(None, tag=...)`. Tagged looks are shown again to a board that reconnects. |
| `kit.schedule(when, call, data=None, id=None, label=None)` | Call `call(kit, data)` at a datetime or in N seconds. Kept across restarts. `data["_late_s"]` says how late it ran. |
| `kit.cancel(id=None, call=None)`, `kit.jobs(call=None)` | Remove or list this skill's scheduled calls. |
| `kit.daily(at, call, days=None)`, `kit.every(minutes, call)` | Recurring calls (set up in `setup`). |
| `kit.config`, `kit.set_config(key, value)` | This skill's section of the local config (plus `general`). |
| `kit.data_dir` | A folder for the skill's own files (in the state folder). |
| `kit.parse_when("at 3")`, `kit.parse_duration("25 minutes")` | Times from speech. |
| `kit.say_time(dt)`, `kit.say_duration(secs)`, `kit.in_hours("22:00-07:00")`, `kit.now()` | Helpers. |
| `kit.skill(name)` | Another skill's `(module, kit)`, to reuse it (the briefing uses `weather`). |
| `kit.boards_online()`, `kit.log(msg)` | |

### eyes.json

Named looks for `kit.eyes()` and `kit.say(..., eyes=...)`. A look is a small description the board plays:

```json
{
  "sun":   {"look": "squint", "particles": "sun", "hue": 48},
  "focus": {"look": "focus", "ring": "countdown", "hue": 265}
}
```

| Field | Values |
|---|---|
| `look` | `happy`, `cheer` (bounce), `surprised`, `squint` (bright sun), `droop` (rain), `shiver` (cold), `calm`, `glow` (attention, for nudges), `focus`, `sleepy`, `asleep` (night), `pout` |
| `particles` | `sun`, `rain`, `snow`, `hearts`, `stars`, `confetti`, `z`, `drops` |
| `hue` | 0–360, the ring and eye colour while the look shows |
| `ring` | `countdown`, with `ring_total` and `ring_left` in seconds: an arc around the eyes that shrinks |
| `secs` | how long it shows (0: until cleared) |

The board keeps its own reactions (touch, tilt, moods) on top, and a board that doesn't know a look shows its
normal face.

## The brain

`hermes-skills/timmy-skills/SKILL.md` tells the brain about the skills; it calls

```
curl -s -X POST http://127.0.0.1:8000/skills/run -H 'Content-Type: application/json' -d '{"text": "timer 10 minutes"}'
```

and gets `{"reply": "..."}` (`null` when nothing fit). `GET /skills` lists the skills with their examples.
