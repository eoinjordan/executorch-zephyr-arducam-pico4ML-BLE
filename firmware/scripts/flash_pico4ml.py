#!/usr/bin/env python3
# Copyright (c) 2026 Eoin Jordan
# SPDX-License-Identifier: Apache-2.0
"""Flash a Zephyr UF2 onto the Pico4ML over USB, no button pressing.

1. If an RPI-RP2 drive is already mounted, use it.
2. Otherwise open the board's USB serial port at 1200 baud and close it: the
   stock Arducam firmware (pico-sdk stdio) and this firmware both treat that
   as "reboot into BOOTSEL".
3. Wait for the RPI-RP2 drive and copy the UF2 onto it. The board reboots
   into the new firmware by itself.

Usage: python firmware/scripts/flash_pico4ml.py [--uf2 build/zephyr/zephyr.uf2] [--port COM11]
"""

import argparse
import pathlib
import shutil
import sys
import time

RP2_VID = 0x2E8A    # Raspberry Pi: the stock Arducam firmware
ZEPHYR_VID = 0x2FE3 # Zephyr's CDC ACM: this firmware


def find_rp2_drive():
    if sys.platform == "win32":
        import ctypes

        buf = ctypes.create_unicode_buffer(261)
        for letter in "DEFGHIJKLMNOPQRSTUVWXYZ":
            root = f"{letter}:\\"
            if ctypes.windll.kernel32.GetVolumeInformationW(root, buf, 261, None, None, None, None, 0):
                if buf.value == "RPI-RP2":
                    return pathlib.Path(root)
        return None
    for base in ("/media", "/run/media", "/Volumes", "/mnt"):
        for p in pathlib.Path(base).glob("**/RPI-RP2"):
            if p.is_dir():
                return p
    return None


def find_port(explicit):
    if explicit:
        return explicit
    try:
        from serial.tools import list_ports  # pip install pyserial
    except ImportError:
        sys.exit("pip install pyserial, or pass --port")
    for p in list_ports.comports():
        if p.vid in (RP2_VID, ZEPHYR_VID):
            return p.device
    return None


def touch_1200(port):
    import serial

    try:
        s = serial.Serial(port, 1200)
        s.dtr = True
        time.sleep(0.3)
        s.close()
    except Exception as e:  # the device usually vanishes mid-close; that is success
        print(f"note: {port}: {e}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--uf2", default="build/zephyr/zephyr.uf2")
    p.add_argument("--port", help="serial port of the running board (auto by VID 0x2E8A)")
    p.add_argument("--timeout", type=float, default=15.0)
    a = p.parse_args()

    uf2 = pathlib.Path(a.uf2)
    if not uf2.is_file():
        sys.exit(f"{uf2} not found; build first")

    drive = find_rp2_drive()
    if drive is None:
        port = find_port(a.port)
        if port is None:
            sys.exit("no RPI-RP2 drive and no RP2040 serial port; hold BOOTSEL and plug in")
        print(f"rebooting {port} into BOOTSEL")
        touch_1200(port)
        deadline = time.time() + a.timeout
        while drive is None and time.time() < deadline:
            time.sleep(0.5)
            drive = find_rp2_drive()
        if drive is None:
            sys.exit("RPI-RP2 drive did not appear")

    dst = drive / uf2.name
    print(f"copying {uf2} ({uf2.stat().st_size} bytes) to {dst}")
    shutil.copyfile(uf2, dst)
    print("done; the board is rebooting into the new firmware")


if __name__ == "__main__":
    main()
