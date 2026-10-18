#!/usr/bin/env python3
"""afterglow's USB serial console, from a Mac or Linux PC.

Waits for the headset's serial port (/dev/cu.usbmodem* on a Mac, /dev/ttyACM* on Linux), prints its
kernel log live and optionally saves it, and sends commands (see system/init/usb.c: bootloader,
reboot, help). Standard library only.

Usage: console.py [--log FILE] [--send CMD]... [--until REGEX] [--duration SECONDS] [--wait SECONDS]
"""
import argparse
import glob
import os
import re
import select
import sys
import time
import tty


def find_port(wait):
    deadline = time.time() + wait
    while True:
        ports = glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*")
        if ports:
            return max(ports, key=os.path.getmtime)
        if time.time() > deadline:
            return None
        time.sleep(0.5)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--log", help="also append everything to this file")
    p.add_argument("--send", action="append", default=[], help="command to send once connected")
    p.add_argument("--until", help="stop once a line matches this regex")
    p.add_argument("--duration", type=float, help="stop after this many seconds connected")
    p.add_argument("--wait", type=float, default=120, help="seconds to wait for the port to appear")
    args = p.parse_args()

    port = find_port(args.wait)
    if not port:
        sys.exit(f"no afterglow console appeared within {args.wait:.0f} s")
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
    tty.setraw(fd)
    log = open(args.log, "a") if args.log else None
    print(f"--- connected to {port} at {time.strftime('%H:%M:%S')}", flush=True)
    for cmd in args.send:
        os.write(fd, (cmd + "\n").encode())

    until = re.compile(args.until) if args.until else None
    start, pending = time.time(), b""
    try:
        while args.duration is None or time.time() - start < args.duration:
            ready, _, _ = select.select([fd], [], [], 0.5)
            if not ready:
                continue
            data = os.read(fd, 4096)
            if not data:
                break
            pending += data
            *lines, pending = pending.split(b"\n")
            for raw in lines:
                line = raw.decode(errors="replace").rstrip("\r")
                print(line, flush=True)
                if log:
                    log.write(line + "\n")
                    log.flush()
                if until and until.search(line):
                    return
    except OSError:
        print("--- disconnected (the headset restarted or was unplugged)", flush=True)


if __name__ == "__main__":
    main()
