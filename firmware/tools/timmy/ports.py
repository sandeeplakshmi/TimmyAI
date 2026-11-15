#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Finds the board's serial port by its USB device (the ESP32-S3's own USB Serial/JTAG).

  tools/timmy/ports.py [SERIAL]    prints the port (SERIAL: the USB serial number, i.e. the MAC)
  tools/timmy/ports.py --list      lists matching ports

Listing ports doesn't open them, so no board is reset. Needs pyserial.
"""

import sys

from serial.tools import list_ports

USJ = (0x303A, 0x1001)


def boards():
    return [p for p in list_ports.comports() if (p.vid, p.pid) == USJ]


def main():
    found = boards()
    if sys.argv[1:2] == ["--list"]:
        for p in found:
            print(p.device, p.serial_number or "")
        return 0
    want = sys.argv[1] if len(sys.argv) > 1 else None
    if want:
        found = [p for p in found if want in (p.serial_number or "") or want == p.device]
    if len(found) != 1:
        sys.stderr.write(f"{len(found)} matching boards on USB; pass the serial number or port\n")
        return 1
    print(found[0].device)
    return 0


if __name__ == "__main__":
    sys.exit(main())
