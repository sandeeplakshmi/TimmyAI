# Media for the README

| File | What | Made with |
|------|------|-----------|
| `eyes-hero.gif` | the eyes cycling through idle, listening, thinking, speaking, love, excited, curious | `firmware/tools/timmy/make_eye_gifs.py` |
| `eyes-idle.gif`, `eyes-listening.gif`, `eyes-thinking.gif`, `eyes-speaking.gif`, `eyes-happy.gif`, `eyes-sleepy.gif` | single moods, looping | same |
| `eyes-ring.png` | the focus timer's countdown ring | same |
| `web-chat.png`, `web-menu.png`, `web-alerts.png`, `web-voice.png`, `web-phone.png`, `setup-page.png` | the web page and the setup page | a demo server on a spare port with a throwaway `TIMMY_HOME` and made-up data ("Alex", "Springfield"), captured with headless Chrome at 2x |

The eye animations are rendered on a computer from the firmware's own drawing code
(`avatar/timmy_eyes.c`), so they match the board exactly. Regenerate them with:

    cd firmware && python3 tools/timmy/make_eye_gifs.py

**Never** capture screenshots from your own running Timmy: use demo data, and check every
frame for names, addresses, Wi-Fi names, host names and notifications (docs/RELEASING.md).

## Shot list for the real board (to record on a phone)

Save as `board-*.mp4` or `.gif` (under ~10 MB each; GitHub shows MP4s from the repo as links,
so also upload the main clip to the README through GitHub's editor if you want it inline).

1. **Hero**: the board on a desk, eyes idling and blinking, then a hold-to-talk question
   and the spoken answer with captions. 15 to 30 s.
2. **Reactions**: tap (happy), stroke up and down (pet), a flurry of pokes (dizzy), tilt
   the board (eyes roll). 10 to 15 s.
3. **Close-up still**: the round screen straight on, eyes open.

Record both vertical (for the phone view) and horizontal. Before committing: no
notifications on the phone, no other screens, nothing readable in the background, and
mute or remove audio that contains anyone's voice besides Timmy's.
