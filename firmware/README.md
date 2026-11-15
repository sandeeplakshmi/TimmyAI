<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

Modified by the Timmy contributors, 2026
SPDX-License-Identifier: Apache-2.0
-->

# Timmy firmware

Firmware for Timmy's desk gadget: the **Waveshare ESP32-S3-Touch-AMOLED-1.75C**
(ESP32-S3, 32 MB flash, 8 MB PSRAM, round 466 px AMOLED, mic, speaker, IMU).
Hold the top button to talk and let go to send; Timmy answers out loud, one
sentence at a time, with captions under its neon eyes. Press again to cut a
reply short.

The board talks to your own Timmy server (`server/gadget_server.py`) over a
WebSocket. Nothing about your network or your server is built into the image:
the server address and the device key are set over USB serial and kept in the
board's NVS.

## Build

Needs [ESP-IDF v6.0.3](https://docs.espressif.com/projects/esp-idf/en/v6.0.3/esp32s3/get-started/).

    cd firmware
    . ~/esp/esp-idf-v6.0.3/export.sh      # wherever your ESP-IDF is
    idf.py build

The top-level `CMakeLists.txt` picks the configuration layers by itself:
`sdkconfig.defaults`, `devices/sdkconfig.timmy` and
`devices/sdkconfig.timmy-waveshare-s3-175c`. Components from the ESP Component
Registry (LVGL, the Waveshare board support package, ...) are fetched into
`managed_components/` on the first build.

Private add-on components (an extra avatar, for example) are picked up from
`$TIMMY_EXTRA_COMPONENTS` or `~/.timmy/firmware-extras`; see
[`docs/CUSTOMIZING.md`](../docs/CUSTOMIZING.md). Set `TIMMY_EXTRA_COMPONENTS=`
(empty) for a plain build when that folder exists.

## Flash

    ../scripts/flash.sh             # finds the board on USB, builds if needed, flashes, waits for it
    # or by hand:
    idf.py -p /dev/cu.usbmodemXXXX flash

Flashing doesn't erase NVS, so Wi-Fi and the server settings survive.

## Set it up

1. **Wi-Fi**: on the board's screen, open Settings > Wi-Fi and pick your network
   (2.4 GHz). The board keeps the networks it has joined.
2. **Server and key**: with the board on USB, run `scripts/provision-board.sh`
   on the Mac that runs Timmy. It makes a device key and sends the server
   address and key to the board (the key is never shown). Or by hand: make a
   device key,

       python3 server/gadget_server.py --new-key timmy-desk

   then type these into a serial terminal (115200 baud; `idf.py monitor` works):

       >timmy.host=<your Mac's LAN address>    (same as >timmy.url=ws://<address>:8790/gadget)
       >timmy.key=<the 64 hex characters it printed>
       >timmy.device=timmy-desk
       >timmy                                   (shows the settings and whether it's connected)

   Optional: `>timmy.fallback=wss://<host>/gadget` is used when the home
   address doesn't answer (the server can also send one; see
   `TIMMY_GADGET_FALLBACK_URL` in `.env.example`).

The key never goes over the network: the server sends a nonce and the board
answers with HMAC-SHA256(key, nonce). On `ws://` the audio itself is not
encrypted, so use plain `ws://` on your own network only and `wss://` for
anything else.

## Layout

- `main/`: the connection core (Wi-Fi, BLE setup, credentials, status) from
  the upstream SDK's Home Link.
- `components/timmy/`: the UI (LVGL), audio, buttons, IMU, settings, the board
  driver (`boards/board_waveshare_s3_175c.c`) and the server link
  (`timmy_chat_server.c`).
- `avatar/timmy_eyes.c`: Timmy's face. `components/timmy/timmy_avatar.h` is the
  avatar registry add-ons can extend.
- `components/noise_core`, `components/product_info`, `components/minimp3`:
  support libraries.
- `tools/timmy/`: `board.sh` (build or flash), `ports.py`, `monitor.py`.

## License

Apache License 2.0 (see `../LICENSE` and `../NOTICE`). Files from the upstream
SDK keep their original copyright headers; `components/minimp3` is CC0-1.0
(see its `LICENSE`). Components fetched at build time keep their own licenses.
