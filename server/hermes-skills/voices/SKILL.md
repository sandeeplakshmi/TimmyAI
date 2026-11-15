---
name: voices
description: "List Timmy's voices (Kokoro natural + Piper) and switch the voice."
version: 1.0.0
author: the Timmy contributors
license: Apache-2.0
platforms: [macos]
metadata:
  hermes:
    tags: [voice, tts, kokoro, piper, speech]
    category: productivity
    requires_toolsets: [terminal]
---

# Voice

Timmy speaks with **Kokoro** (natural-sounding, local, free; default voice `michael`) or **Piper** (lighter, more robotic). Both run on the Mac. This skill lists all English voices and switches between them; `set` updates Hermes' config (`tts.provider` = `kokoro` or `piper`), so the terminal, the web page (http://localhost:8000) and the phone app all use the new voice. You can also audition voices in the web page's VOICE panel.

Kokoro voices (marked "natural", preferred): US male michael, adam, eric, liam, onyx (deep), fenrir, puck; British male george, lewis, fable; US female heart, bella, nicole, sarah; British female emma, isabella. Say e.g. `set michael`, `set george`, `set heart`.

Helper script (run it with the terminal tool, using Hermes' own Python so Piper is available):

```
HPY=$(ls -td ~/.hermes/installs/*/environments/*/venv | head -1)/bin/python
"$HPY" "$TIMMY_REPO/server/hermes-skills/voices/scripts/timmy_voice.py" <command>
```

Commands: `list`, `current`, `set <name or id>`, `test`, `say <text>`.

## When to Use

- "What voices do you have?" / "list voices" → `list`
- "Which voice are you using?" → `current`
- "Switch to ryan" / "use the British voice" / "change your voice to cori high" / "go back to your original voice" → `set <what they said>`
- "Say something in your voice" → `test`

## Procedure

1. For a list: run `list` and read back the options briefly, grouped as US female, US male, British, Scottish. Mention quality levels only if asked (high sounds best, low is fastest).
2. To switch: run `set` with the voice words they used, e.g. `set british`, `set ryan`, `set cori high`, `set en_GB-alan-medium`. The script understands names plus: british / british male → alan, british female → cori, scottish → alba, northern, southern, male → ryan, female / original / default → lessac. A voice that isn't downloaded yet downloads once (20–110 MB), so tell them it may take a moment.
3. The script plays a test line on the Mac speakers in the new voice and prints `{"ok": true, "voice": "<id>"}`. Confirm with the voice name in one short sentence.
4. If it prints `"ok": false`, say the voice wasn't found and offer two or three close options from `list`.

## Pitfalls
- Only Piper voices. Don't switch the TTS provider or suggest paid voices (ElevenLabs, OpenAI): Timmy uses free/local services only.
- Don't edit `~/.hermes/config.yaml` by hand; the script uses `hermes config set`.
- In the CLI, the new voice applies from the next reply. If it doesn't, tell them to restart `hermes`.

- Voice input is transcribed by local Whisper and often garbles "Timmy" / "Hey Timmy" (e.g. "Hair miss", "HKR", "Care"). Treat a garbled greeting-like phrase as addressing Timmy and answer naturally; don't ask what it meant or reinterpret it as a request.
- When asked what skills are available, call skills_list first and answer from it. The skill set changes mid-session, and answering from memory or the roadmap led to repeated contradictory corrections.

## Verification
- `current` prints the new voice id.
