# Timmy: extraction report (2026-11-15)

How this repository was made from two private repositories, what was kept,
dropped and renamed, how it was verified, and what is still open. This file
is the provenance record, so it is one of the places allowed to name the
upstream project.

## Sources (read only)

Both were read only through `git archive` of committed content. Neither
repository was modified, checked out, stashed or committed to.

- **Firmware**: the Muse Gadgets SDK fork, branch `skills` (8b57e4d). Taken:
  `esp32/`, plus the root `LICENSE`, `CONTRIBUTING.md` and
  `CODE_OF_CONDUCT.md`. The last two were rewritten for Timmy.
- **Server and web**: the personal assistant repo, branch `skills` (dc162d4).

This repo has a fresh `git init` with no history from either source. It lives
on branch `main`, local only: nothing was pushed and no GitHub repo was made.

## Kept

- **`firmware/`** (from `esp32/`), slimmed to the Waveshare
  ESP32-S3-Touch-AMOLED-1.75C Timmy build:
  - `main/` (the Home Link core), `components/timmy` (was `components/muse`),
    `noise_core`, `product_info`, `minimp3`
  - `avatar/timmy_eyes.c`
  - `devices/sdkconfig.timmy` and `devices/sdkconfig.timmy-waveshare-s3-175c`
  - `devices/guides`, `partitions_timmy.csv`, `cmake/`
  - `tools/timmy/` (board.sh rewritten, ports.py rewritten, monitor.py kept)
- **`server/`**:
  - `timmy_server.py` (was `ui/sai_server.py`) and `gadget_server.py`
  - `skillkit.py` and the local skills in `skills/` (hello, focus, weather,
    reminders, notes, buddy, briefing)
  - Generic brain skills in `hermes-skills/`: timmy-skills, voices, research,
    diy-helper, mac-control, daily-briefing
  - The login-service scripts in `autostart/` (the gateway hook, install,
    uninstall)
- **`web/`**: `index.html`, `avatar.js`, `manifest.json`, the icons, and the
  three.js and TalkingHead vendors.
- **`LICENSE`**: Apache-2.0, unchanged.

## Dropped

- **Firmware, other hardware**:
  - 80+ other board sdkconfigs and 7 partition tables
  - every other board driver, and e-ink/FreeInk
  - video, camera and the decoders: `tinyh264`, `libvpx_vp8`, `camera`,
    `muse_vstream`
  - fonts (CJK, Latin, Vietnamese) and the power-test code
  - the voice-board drivers (Voice PE, reSpeaker), e-paper status and the
    pipeline bench
- **Firmware, code paths no longer used**:
  - The Hatch cloud chat (`muse_chat.c`, `muse_chat_session.cpp`,
    `muse_account_api.c`, realtime voice calls)
  - `muse_chat_link.c` and `sdk_token_console.c`
  - The Muse avatar (`avatar/muse_pixel.c`), the avatar picker, `happy_anim`
    plus `jollybot.gif`, `pixel_font`
- **Firmware, docs and tools**:
  - `docs/`, `AGENTS.md`, `tests/` (host harnesses for removed code),
    `simulator/`
  - `components_mosaico/`, the coexistence and power tools
  - the third-party license texts for code that's gone (xiaozhi-esp32,
    Seeed_GFX2)
- **Never copied**: `dev_signing_key.pem` and any other key, `build-*/`,
  `managed_components/`.
- **Other source folders**: `android/`, `ios/`, `linux/`, `protocols/`,
  `skills/`. `web-flasher/` too, because it centres on the upstream SDK-token
  flow and many boards; `scripts/flash.sh` replaces it.
- **Server, personal content**:
  - `topics/`, `CLAUDE.md`, `SETUP_PROMPT.md`
  - the deal-watcher, finances, topics and dev-helper skills
  - the personal price-watch code in the web server and page
  - memory, alerts data, `.env`, keys
- **Server, avatars**: the TalkingHead sample avatars (`*.glb`). Their CC
  BY-NC license doesn't fit Apache-2.0; `web/vendor/avatars/README.md` says
  where to get them.
- **Tailscale and cloudflared scripts**: moved to the private overlay
  (below).

## Renamed

Every remaining "Muse"/"muse"/"MUSE" became Timmy/timmy/TIMMY, done with a
script over file names, directories, symbols, Kconfig, sdkconfig names, build
targets, UI strings, comments and docs. Examples:

- `components/muse` → `components/timmy`
- `CONFIG_MUSE_*` → `CONFIG_TIMMY_*`
- `MuseGadget-XXXXXX` → `Timmy-XXXXXX`
- `muse_chat_timmy.c` → `timmy_chat_server.c`
- `musegadgets::noise` → `timmygadgets::noise`

Upstream cloud host names in code paths Timmy doesn't use were replaced with
`*.example.invalid`. `git ls-files | grep -i muse` finds nothing.
`scripts/check-private.sh` greps the contents and is clean: "muse" appears
only in `NOTICE`, the `README.md` credit and this file.

## Firmware changes beyond slimming

- **Simplified build**: `unifdef` removed the dead `#if` branches for other
  boards and features, and the Kconfig is pruned to the one board. Plain
  `idf.py build` now picks the configuration layers by itself.
- **Signing and OTA off**: app signing is disabled because no key ships with
  the repo, and OTA is off too (`HOMEHUB_OTA_ENABLED=n`). The support
  bug-report path is off.
- **Fallback server URL**: the away-from-home address is now a generic setting
  (`>timmy.fallback=` over serial, or the server sends `{"type":"fallback"}`).
  No tunnel vendor is named.
- **Avatar registry** (`components/timmy/timmy_avatar.h`): Timmy's eyes are
  built in. Add-on components can register more faces through weak
  `timmy_avatar_extras()`. Settings > Avatar appears only when there is more
  than one face.
- **Add-on components**: the top-level `CMakeLists.txt` adds `EXTRA_COMPONENT_DIRS`
  from `TIMMY_EXTRA_COMPONENTS`, or from `~/.timmy/firmware-extras` when it
  exists.
- **Firmware version on connect**: the board sends its version when it
  connects. The server records it in `$TIMMY_HOME/gadget-status.json`, and
  doctor and `flash.sh` report it.
- **Version**: `version.txt` is 0.1.0.

## Server changes

- **Personal data moved out**: all of it now lives in `$TIMMY_HOME` (default
  `~/.timmy`): sessions, briefings, alerts, allowed hosts, device keys
  (`keys/gadget-keys.json`), skill state and settings (`skill-state/`).
  Nothing personal remains in code. The weather city and location are blank
  by default.
- **First-run setup page**: `web/setup.html`, served at `/setup/status` and
  `/setup/save` (`server/setup_store.py`). The Claude key goes to Hermes' own
  `.env` (mode 600) and is never echoed back. A local model or name goes to
  `$TIMMY_HOME/.env`.
- **Add-on hook** (`server/addons.py`): loads `$TIMMY_HOME/addons/*/addon.py`
  and calls `register(api)`. Hooks available: `FALLBACK_PROVIDERS` (gadget),
  `CARD_PROVIDERS` and `ALERT_PROVIDERS` (web page). The alerts panel shows
  watches and listings only when an add-on provides them.

## Licensing

- `LICENSE` is unchanged (Apache-2.0).
- **`NOTICE`**: the requested attribution plus the bundled components:
  - minimp3 (CC0-1.0)
  - three.js (MIT)
  - TalkingHead (MIT)
  - fetched-at-build components, with their own licenses
- **Upstream files**:
  - Every upstream `Copyright (c) Meta Platforms, Inc. and affiliates.` line
    is unchanged.
  - The 58 upstream files changed for Timmy carry `Modified by the Timmy
    contributors, 2026` and an SPDX line under the original header.
  - The 56 untouched upstream files are byte-identical; their SHA-256 sums
    are in `scripts/upstream-manifest.txt`.
- **New files**: they carry `SPDX-License-Identifier: Apache-2.0` and
  `Copyright 2026 the Timmy contributors`. The brain-skill metadata now says
  Apache-2.0 (it said MIT before).

## Guardrails

- **Personal data stays out of the repo**: it lives in `~/.timmy`
  (`TIMMY_HOME`), and only `.env.example` (blank values) and
  `templates/timmy-home/` ship.
- **`.gitignore` is strict**: `.env*` except `.env.example`, `*.pem`,
  `*.key`, `keys/`, `data/`, `memory/`, `topics/`, `finance/`, `alerts/`,
  `briefings/`, `*.db`, `*.sqlite`, `build*/`, `managed_components/`,
  `sdkconfig`, logs.
- **`scripts/check-private.sh`** runs on all tracked files, or only staged
  ones with `--staged`. It fails on:
  - a gitleaks finding
  - any string from `~/.timmy/denylist.txt` (never printed)
  - muse, cloudflared, trycloudflare, tailscale or ts.net outside `NOTICE`,
    `README.md` and `REPORT.md`
  - absolute home paths (`/Users/<name>/…`)
- **`scripts/check-licenses.sh`** checks that:
  - `LICENSE` and `NOTICE` exist
  - every source file has an SPDX or Apache header
  - the upstream copyright lines are unchanged
  - any upstream file that changed carries the "Modified by" line (checked
    against the manifest)
  - third-party license texts are present and listed in `NOTICE`
- **Pre-commit hook**: `scripts/pre-commit`, installed by
  `scripts/install-hooks.sh` as a thin wrapper. It runs both checks and
  blocks new source files that have no license header.
  `.pre-commit-config.yaml` covers the same checks for the pre-commit
  framework (not installed here).
- **CI**: `.github/workflows/checks.yml` runs gitleaks over the full history,
  both checks and a Python syntax check on every push and PR. It is only a
  file; nothing was pushed.
- **Docs**: `docs/RELEASING.md` has the checklist and the "never" list.
  `CONTRIBUTING.md` has the SPDX rule and the no-third-party-branding rule.
  `docs/CUSTOMIZING.md` describes the overlay in generic terms.
- **Denylist**: `~/.timmy/denylist.txt` is created on this Mac (mode 600,
  outside the repo), with the known personal strings. It's not reproduced
  here.
- **Gitleaks false positive**: `.gitleaksignore` holds one fingerprint, the
  RFC 6455 sample WebSocket key in `firmware/main/noise_upgrade.h`.

**Hook proof:** in this repo, a commit containing a fake GitHub token and a
denylisted host name was blocked: gitleaks found `github-pat`, and the
denylist and private-network checks both fired. The test file was unstaged
and deleted, and no commit was created. A new `.py` file without a header and
a file with "muse" in its path were blocked the same way.

## Installer, doctor, flashing, agents

- **`./install.sh`** takes `--yes`, `--dry-run`, `--json`, `--no-services`,
  `--no-open` and `--with-firmware-tools`. It is idempotent and has distinct
  exit codes, 10 to 18. It:
  - checks and installs prerequisites (Homebrew, Python, Hermes Agent,
    voice)
  - registers the repo's and `~/.timmy`'s brain skills with Hermes
    (`scripts/hermes-skill-dirs.py`)
  - creates `~/.timmy` from templates, and never touches an existing one
  - sets up the gateway login service and web hook
  - lists the add-ons
  - opens the setup page
- **`./install.sh doctor [--json]`** checks the prerequisites, the folder,
  the brain, the gateway, the web page, voice, the hooks, ESP-IDF, the board
  on USB, the device key and the board's last connection (with its firmware
  version).
- **`./install.sh flash`** (`scripts/flash.sh`):
  - detects the board by USB VID 0x303A
  - while waiting, shows a spinner, then after 30 s hints about the cable,
    port, download mode and serial monitors; after 3 min it exits 20
    BOARD_NOT_FOUND
  - lists several boards and asks, or takes `--port`
  - asks before flashing, or takes `--yes`
  - can back up first with `--backup`
  - waits for the board to reach the server, with hints
  - emits JSON states with `--json`
  - builds with the add-ons when `~/.timmy/firmware-extras` exists
- **`scripts/provision-board.sh`** sends the server address and a new device
  key to the board over serial without ever printing the key.
- **Backup and restore**: `scripts/backup-private.sh` (encrypted zip with
  7-Zip, or a local git repo that excludes `keys/` and `.env`) and
  `scripts/restore-private.sh`, documented in `docs/RESTORE.md`.
- **Agent instructions**: `AGENTS.md`, plus `CLAUDE.md` (`@AGENTS.md`),
  `GEMINI.md` and `.github/copilot-instructions.md`. The README has an
  "Install with an AI assistant" prompt.

## Verification

- **Baseline**: the unmodified source (Timmy config, ESP-IDF v6.0.3) built
  first.
- **Public build** (`TIMMY_EXTRA_COMPONENTS= idf.py build`) in `firmware/`:
  - clean: no errors and no warnings from Timmy code
  - `timmy-gadget.bin` is 2.10 MB, and `strings | grep -i muse` finds 0
    matches
- **Extras build** (`idf.py -B build-extras build`, using
  `~/.timmy/firmware-extras/avatar_muse`): clean, 2.12 MB, and it contains
  `avatar_muse_render`, `timmy_avatar_extras` and the "Muse" face name.
- **Server, offline checks**: `py_compile` passes on all Python.
- **Server, runtime test**: I started `timmy_server.py` on spare port 18000
  with a throwaway `TIMMY_HOME`/`HERMES_HOME`, so it never touched the live
  gateway or ports 8000, 8642 and 8790. `/`, `/cards`, `/alerts`, `/skills`
  and `/skills/run` ("note: test") all answered correctly.
- **Installer in a temp HOME** (`--yes --no-services --no-open`, plain and
  `--json`): exit 0. It created `~/.timmy` with modes 700/600, and a re-run
  was idempotent.
- **Board not found**: tested in simulation (`TIMMY_SIMULATE_NO_BOARD=1`,
  35 s timeout). It printed the hints at 30 s and exited 20. The real board
  stayed plugged in.
- **AI-agent install test**: a fresh agent was given only the README prompt,
  in a temp HOME, constrained to `--no-services` so it couldn't touch the
  live services.
  - It completed the install (exit 0), ran doctor, installed the hooks, ran
    both checks (clean) and stopped before flashing.
  - Doctor's remaining required failures were the brain key (the human's
    job) and the services the harness wouldn't let it start.
  - It found eight issues, all fixed in commit 442f779: a false ✓ for a
    broken Hermes, a hardcoded gateway port, incomplete flash exit codes in
    AGENTS.md, no key-safe provisioning, vague voice advice, `--no-open`
    undocumented, and doctor fixes that start services without saying so.

## Scan results

- **gitleaks 8.30.1** (installed with brew): clean over the tracked files,
  after the one RFC-sample ignore. trufflehog wasn't needed.
- **Denylist and branding grep** (`scripts/check-private.sh`): clean. During
  the work it caught two leftovers, a greeting with the owner's name in
  `timmy_voice.py` and a LAN IP in the old firmware README; both were fixed.
- **Scrubbed**:
  - LAN IPs in firmware comments
  - personal greetings and system prompts
  - the city and coordinates
  - personal launch-agent labels and absolute home paths
  - the tailnet and tunnel names

## Private overlay (`~/.timmy`, never committed)

- **`firmware-extras/avatar_muse/`**: the original Muse face from the
  `skills` branch, with Meta's header and a "Modified" line. It registers as
  an extra face.
- **`addons/tailscale/` and `addons/cloudflared/`**: the original scripts.
  `cloudflared/addon.py` gives the gadget its fallback URL from the tunnel
  log.
- **`denylist.txt`**: the private strings checked before every commit.

## Uncertain or open

- **The upstream connection core is still compiled.** That covers VM API,
  phone pairing over BLE, the Noise tunnel and link pairing. `main/app.c`'s
  connection manager is built around them, so removing them is a larger
  refactor than this pass. In Timmy mode they stay idle unless someone pairs
  the board with the upstream phone app, and the cloud host names point at
  `*.example.invalid`, so nothing reaches the upstream cloud. This is a good
  follow-up.
- **Some identifiers sound odd after the rename**: `timmy_hatch_*`,
  "Timmy Home Link" in comments. They work but could be tidied.
- **The device guides** (`devices/guides/timmy.md`, `main/guide.md`) still
  describe the upstream agent's command set.
- **The denylist is incomplete.** The home Wi-Fi SSID (hidden by macOS
  privacy) and family names weren't found in committed files. Add them to
  `~/.timmy/denylist.txt` yourself.
- **Commit identity**: commits use the neutral "Timmy contributors" identity
  (a GitHub noreply address). Amend it if you want your own name on them.
- **localStorage keys** in the web page still use a short `kr.` prefix; it's
  harmless.
- **The fresh macOS user account test** wasn't done (it needs an admin GUI
  step). The temp-HOME tests stand in for it, and they can't exercise the
  per-user launch agents.
- The GitHub repo URL is set as `origin` but kept out of the repo files: it
  contains the owner's login, which is on the private denylist. Put it into
  README.md/AGENTS.md (replacing `<repo URL>`) once that denylist entry is
  narrowed. Nothing has been pushed.

## Not done yet (next session)

- **Switch the live Timmy to this folder.** Steps:
  - copy the data into `~/.timmy`
  - put the personal skills in `~/.timmy/hermes-skills`, plus a private
    add-on for the deal-watcher and topics panels
  - point the gateway hook, `skills.external_dirs`, the cron job paths and
    `SOUL.md` at the new folder
  - restart and verify
- **Flash the extras build** to the board (report first, keep a `--backup`
  image).
- **Back up `~/.timmy`** as a local git repo (`scripts/backup-private.sh git`).
- The backup zip of the old folders is done:
  `~/Documents/code/WorkSpace/_old-timmy-backup-2026-11-15.zip`, 509 MB,
  integrity-tested, both `.git` folders included.
