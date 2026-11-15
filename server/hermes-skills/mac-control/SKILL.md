---
name: mac-control
description: "Control your Mac: apps, volume, music, Focus, Shortcuts."
version: 1.0.0
author: the Timmy contributors
license: Apache-2.0
platforms: [macos]
metadata:
  hermes:
    tags: [macOS, apps, volume, music, spotify, shortcuts, focus, do-not-disturb]
    category: productivity
    requires_toolsets: [terminal]
---

# Mac control

Control the Mac with `open`, `osascript` and `shortcuts`, using the terminal tool. Keep replies short ("Done, volume is 30%").

## When to Use

- "Open Safari" / "quit Slack" / "what apps are open?"
- "Volume up" / "set volume to 30" / "mute"
- "Play / pause / next song", "what's playing?" (Spotify, otherwise Music)
- "Turn on Do Not Disturb" / "focus mode on/off"
- "Run my <name> shortcut" / "what shortcuts do I have?"

## Procedure

### Apps
- Open: `open -a "<App Name>"`. If the name is not found, run `ls /Applications /System/Applications` and pick the closest match.
- Running apps: `osascript -e 'tell application "System Events" to get name of every application process whose background only is false'`
- Quit: **ask first** ("Quit Xcode? Unsaved work may be lost."), then `osascript -e 'quit app "<App Name>"'`. Never force-quit (`kill`, `killall`, `-9`) unless the user explicitly says "force quit".

### Volume (no permission needed)
- Get: `osascript -e 'output volume of (get volume settings)'`
- Set 0–100: `osascript -e 'set volume output volume 30'`
- Up/down: get, then set ±10 (clamp 0–100).
- Mute / unmute: `osascript -e 'set volume output muted true'` / `false`

### Music
Use Spotify if it is running (`pgrep -x Spotify`), otherwise Music.
- Play / pause / next / previous: `osascript -e 'tell application "Spotify" to playpause'` (also `play`, `pause`, `next track`, `previous track`; same verbs for `"Music"`).
- What's playing: `osascript -e 'tell application "Spotify" to (artist of current track) & " - " & (name of current track)'`
- Spotify may not be installed (`open -a Spotify` says "Unable to find application"). Don't keep retrying: tell the user and offer Apple Music or the Spotify web player (`open https://open.spotify.com`).
- "Play some music" with no query in Music: a bare `play` fails with -1728 ("Can't get artist of current track") when the player is stopped with an empty queue. Use `osascript -e 'tell application "Music" to play library playlist 1'` (check `player state` first), then read back what's playing.
- Search and play a song or artist: open it in Spotify with `open "spotify:search:<url-encoded query>"` and tell them to pick from results.

### Do Not Disturb / Focus
macOS has no direct command for Focus. Use the Shortcuts named **Timmy Focus On** and **Timmy Focus Off** (the user creates them once: Shortcuts app → New Shortcut → "Set Focus" → Do Not Disturb On/Off).
- `shortcuts run "Timmy Focus On"` / `shortcuts run "Timmy Focus Off"`
- If they don't exist (`shortcuts list` doesn't show them), explain how to create them in one sentence.

### Shortcuts
- List: `shortcuts list`
- Run: `shortcuts run "<exact name>"`. The Timmy Focus shortcuts can run without asking. For **any other shortcut, say its name and ask before running it**. Some of their shortcuts call people or show bank balances, so never run one they didn't name, and never run call, message or finance shortcuts without a clear yes.

## Pitfalls
- First use of `osascript` to control an app (Spotify, Music, System Events) triggers a macOS **Automation** prompt for the app running Hermes (Terminal, or Python for the gateway). If a command fails with "Not authorized to send Apple events" (error -1743), tell the user to allow it in System Settings → Privacy & Security → Automation.
- Opening Music with `open -a Music` and immediately sending `play` is not enough; Music may stay `stopped`. Verify with `player state` rather than assuming playback began.
- Don't use keystroke or UI scripting (`System Events` keystroke / click). It needs Accessibility permission, which hasn't been granted.
- Ask before anything destructive: quitting apps, deleting, emptying the Trash, shutting down, restarting, logging out. Don't change system settings beyond volume and Focus.

## Verification
- Volume: read it back after setting.
- Apps: confirm with the running-apps list.
- Music: read "what's playing" after a change.
