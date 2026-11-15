# AGENTS.md: instructions for AI coding agents

Timmy is a personal voice assistant for macOS: a Python web server and gadget
endpoint (`server/`), a web page (`web/`), Hermes Agent as the brain, and ESP-IDF
firmware for a Waveshare ESP32-S3-Touch-AMOLED-1.75C desk gadget (`firmware/`).
Personal data lives outside the repo in `~/.timmy` (`TIMMY_HOME`).

## Install (macOS)

1. `./install.sh --yes` installs prerequisites, creates `~/.timmy` from
   `templates/timmy-home` if missing (never touches an existing one), sets up
   the login service and opens the setup page. Add `--json` for one JSON object
   per step (`{"step", "state", "message"}`), `--dry-run` to preview.
   Exit codes: 10 NOT_MACOS, 11 HOMEBREW_MISSING, 12 PYTHON_FAILED,
   13 HERMES_FAILED, 14 TIMMY_HOME_FAILED, 15 VOICE_FAILED, 16 SERVICES_FAILED,
   17 WEB_NOT_UP, 18 ESP_IDF_FAILED.
2. `./install.sh doctor --json` reports every check with `ok`, `required`,
   `detail` and `fix`. Fix what you can, run it again, and tell the human about
   the rest.
3. The human enters the Claude API key (or picks a local model) on
   http://localhost:8000/setup.html. **Never ask for, read, paste, store or
   print API keys or other secrets.** Non-secret options can be set with env
   vars (`TIMMY_HOME`, `TIMMY_PORT`, `TIMMY_VOICE`).

## Verify

- `./install.sh doctor` passes its required checks.
- `scripts/check-licenses.sh` and `scripts/check-private.sh` print "clean".
- Server syntax: `python3 -m py_compile server/*.py`.
- Firmware: `cd firmware && idf.py build` (ESP-IDF v6.0.3; `./install.sh --with-firmware-tools` installs it).

## Firmware and the board

- **Ask the human before flashing.** Then run `./install.sh flash --yes --json`
  (it emits `waiting_for_board`, `board_found`, `building`, `flashing`,
  `flashed`, `waiting_for_wifi`, `connected`, `error`).
- If the board isn't found (exit 20 BOARD_NOT_FOUND), **ask the human to plug
  it in; don't loop silently.** Hints to pass on: use a USB-C *data* cable
  (charge-only cables don't work), another port, no hub; download mode = hold
  BOOT (bottom button), tap power/reset, release BOOT; close other serial
  monitors (exit 22 PORT_BUSY). Several boards: exit 21, ask which and pass `--port`.
- After flashing, relay the Wi-Fi step to the human (Settings > Wi-Fi on the
  board, 2.4 GHz, same network as the Mac). Exit 25 NOT_CONNECTED means the
  board didn't reach the server: same Wi-Fi, server address and key set over
  USB serial (`firmware/README.md`), macOS firewall prompt for Python.

## Rules

- Never commit anything from `~/.timmy`, `.env` files, keys, logs, build output
  or `managed_components/`. Private add-ons go in `~/.timmy` (`docs/CUSTOMIZING.md`).
- Run `scripts/install-hooks.sh` once; the pre-commit hook runs the checks. Never
  bypass it (`--no-verify`).
- **Ask the human before** starting or installing services, flashing the board,
  or anything that changes their system outside this folder (`--yes` on the
  installer counts as their consent only if they asked you to run it).
- Don't push or publish anything unless asked.

## License rules

- Apache-2.0. New files start with `SPDX-License-Identifier: Apache-2.0` and
  `Copyright 2026 the Timmy contributors`.
- Files from the upstream SDK keep their original copyright headers exactly;
  when you change one, add `Modified by the Timmy contributors, 2026` under the
  header.
- Don't add the upstream vendor's or product's names, logos, avatars or assets.
  Attribution lives only in `NOTICE` and the README credits.
