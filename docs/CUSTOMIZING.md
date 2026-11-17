# Customizing Timmy without touching the repository

The repository is the same for everyone. Your own data, settings and private
extras go in your Timmy folder, `~/.timmy` (override with `TIMMY_HOME`). The
code reads from there; nothing in it is ever committed.

## Settings

- `~/.timmy/.env`: non-secret settings (see `.env.example`). `start-timmy.sh`
  loads it.
- `~/.timmy/skill-state/config.yaml`: local skill settings (city, quiet hours,
  briefing time; `server/skills/config.example.yaml` lists every key). Voice
  commands like "set my city to Denver" write it for you.
- `~/.timmy/allowed-hosts.txt`: extra host names the web page answers to.
- The Claude API key lives in Hermes' own env file; enter it on the setup page.

## Calendar

Timmy reads Apple Calendar through EventKit in one query
(`server/hermes-skills/daily-briefing/scripts/calendar_events.py`), for any day
or range: "any events tomorrow?", "what's on this week?", "am I free Friday?".
It is read-only and skips holiday, birthday and other subscribed calendars
unless you ask about them. It needs **Calendars** access (System Settings >
Privacy & Security > Calendars) for the app running Hermes; the gateway runs
under `osascript`. The script never pops up the macOS prompt on its own:
`calendar_events.py --request-access` asks once, when you choose to run it.

## Server add-ons

Put a folder in `~/.timmy/addons/<name>/` with an `addon.py` that defines
`register(api)`. The web server calls it once at start (`server/addons.py`):

```python
def register(api):
    # api.server: the web server module; api.gadget: the gadget endpoint (or None)
    # api.home: the Timmy folder; api.log(msg): write to the server log
    if api.gadget is not None:
        api.gadget.FALLBACK_PROVIDERS.append(lambda: "wss://example.com/gadget")
```

An add-on folder can also hold its own scripts and launch agents (for remote
access, backups, ...). A failing add-on is logged and skipped.

## Desk gadget fallback address

The gadget tries its home address first. When that doesn't answer it uses a
fallback `wss://` URL, if it has one: set it on the board (`>timmy.fallback=`
over USB serial), or let the server send it (`TIMMY_GADGET_FALLBACK_URL`,
`~/.timmy/gadget-fallback-url.txt`, or an add-on's `FALLBACK_PROVIDERS`).

## Firmware add-ons

Any ESP-IDF component in `~/.timmy/firmware-extras/` (or the folders listed in
`TIMMY_EXTRA_COMPONENTS`, separated by `:`) is built into the firmware. To add
a face to Settings > Avatar, define `timmy_avatar_extras()` from
`components/timmy/timmy_avatar.h` and link the component whole:

```cmake
idf_component_register(SRCS "my_face.c" PRIV_REQUIRES timmy WHOLE_ARCHIVE)
```

```c
#include "timmy_avatar.h"
static const timmy_avatar_t FACE = { .name = "My face", .render = ..., .set_size = ...,
                                     .scale = ..., .accent = ... };
static const timmy_avatar_t *const LIST[] = { &FACE };
size_t timmy_avatar_extras(const timmy_avatar_t *const **list) { *list = LIST; return 1; }
```

`scripts/flash.sh` builds with the add-ons when the folder exists (in
`firmware/build-extras`); `--public` builds without them.

## Keeping it private

- Add your private strings (Wi-Fi name, addresses, names, e-mails) to
  `~/.timmy/denylist.txt`: the pre-commit hook blocks any commit containing them.
- Back up the folder with `scripts/backup-private.sh` (encrypted zip, or a local
  git repo with keys and `.env` excluded). See `docs/RESTORE.md`.
