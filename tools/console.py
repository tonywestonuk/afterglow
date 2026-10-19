#!/usr/bin/env python3
"""afterglow's USB serial console, from a Mac or Linux PC.

The headset shows up as two serial ports (/dev/cu.usbmodem* on a Mac, /dev/ttyACM* on Linux): the
first streams its kernel log and takes commands (see system/init/usb.c: bootloader, reboot, help),
the second is a BusyBox shell. Standard library only.

Usage: console.py [--log FILE] [--send CMD]... [--until REGEX] [--duration S] [--wait S]   kernel log
       console.py --shell [--send CMD]...                                                   the shell
       console.py --run 'COMMAND'          run one shell command, print its output, exit with its status
       console.py --wifi                   join a Wi-Fi network (asks here; needs bringup.sh wifi)
       console.py --push LOCAL:REMOTE      copy a small file to the headset (its RAM, unless REMOTE is
                                           on a mounted partition)
       console.py --pair                   let this computer's Claude Code use afterglowd on the headset
"""
import argparse
import base64
import getpass
import glob
import locale
import os
import re
import secrets
import select
import shutil
import socket
import subprocess
import sys
import threading
import time
import tty


def find_port(wait, shell):
    """The log port is the first of afterglow's ports, the shell the second (macOS: ...1, ...3)."""
    deadline = time.time() + wait
    while True:
        ports = sorted(glob.glob("/dev/cu.usbmodemafterglow*") + glob.glob("/dev/ttyACM*"),
                       key=lambda p: (len(p), p))
        if len(ports) >= (2 if shell else 1):
            return ports[1] if shell else ports[0]
        if time.time() > deadline:
            return None
        time.sleep(0.5)


def run(fd, command, timeout, emit=None):
    """Run one command in the shell; print its output (or pass each line to emit); return its exit status.

    The command is wrapped in markers it prints itself: echo "__AG""_START__"; ...; echo "__AG""_DONE__$?".
    The shell's echo of the typed line only ever contains the split, quoted form (and wraps long
    lines unpredictably), so the printed markers bracket the output reliably.
    """
    os.write(fd, f'\necho "__AG""_START__"; {command}; echo "__AG""_DONE__$?"\n'.encode())
    done = re.compile(r"__AG_DONE__(\d+)")
    emit = emit or (lambda line: print(line, flush=True))
    pending, started, deadline = b"", False, time.time() + timeout
    while time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.5)
        if not ready:
            continue
        pending += os.read(fd, 4096)
        *lines, pending = pending.split(b"\n")
        for raw in lines:
            line = raw.decode(errors="replace").rstrip("\r")
            if not started:
                started = line.endswith("__AG_START__")
                continue
            m = done.search(line)
            if m:
                if line[:m.start()]:
                    emit(line[:m.start()])   # output without a final newline
                return int(m.group(1))
            emit(line)
    print(f"--- no result within {timeout:.0f} s", file=sys.stderr)
    return 124


def push(fd, local, remote):
    """Copy a file through the shell: base64, in pieces that fit comfortably on a command line."""
    with open(local, "rb") as f:
        data = base64.b64encode(f.read()).decode()
    staged = remote + ".b64"
    if run(fd, f": > '{staged}'", 30):
        return 1
    for i in range(0, len(data), 768):
        if run(fd, f"printf %s '{data[i:i + 768]}' >> '{staged}'", 30):
            return 1
    return run(fd, f"base64 -d '{staged}' > '{remote}' && rm '{staged}' && md5sum '{remote}'", 30)


def resolve(host, timeout=5):
    """The host's IPv4 address, or None if it doesn't resolve within timeout seconds."""
    found = []

    def lookup():
        try:
            found.append(socket.gethostbyname(host))
        except OSError:
            pass
    t = threading.Thread(target=lookup, daemon=True)
    t.start()
    t.join(timeout)
    return found[0] if found else None


def pair(fd):
    """Pair this computer with afterglowd on the headset, over USB: the link only someone holding the
    headset can use. The token is made once and kept in ~/.config/afterglow/token; the headset keeps
    it in RAM only, so pair again after it restarts. The first time, Claude Code is told about the
    server (for the project in the current directory).
    """
    path = os.path.expanduser("~/.config/afterglow/token")
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), mode=0o700, exist_ok=True)
        with os.fdopen(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "w") as f:
            f.write(secrets.token_urlsafe(32) + "\n")
    with open(path) as f:
        token = f.read().strip()
    if run(fd, f"(umask 077; mkdir -p /run/afterglowd; echo {token} > /run/afterglowd/token)", 30):
        return 1
    print("the headset now accepts this computer's token")

    # afterglow.local (mDNS) survives DHCP giving the headset a new address; fall back to the address
    host = "afterglow.local"
    if not resolve(host):
        lines = []
        run(fd, "ip -4 -o addr show wlan0", 30, lines.append)
        m = re.search(r"inet (\d+\.\d+\.\d+\.\d+)/", " ".join(lines))
        if not m:
            print("the headset has no Wi-Fi address yet: join a network (--wifi), then pair again")
            return 1
        host = m.group(1)
        print(f"afterglow.local doesn't resolve here; using the headset's address, {host}")
    url = f"http://{host}:7070/mcp"
    if not shutil.which("claude"):
        print(f"to use it from Claude Code: claude mcp add --transport http afterglow {url} "
              '--header "Authorization: Bearer $(cat ~/.config/afterglow/token)"')
    elif subprocess.run(["claude", "mcp", "get", "afterglow"], capture_output=True).returncode == 0:
        print(f"Claude Code already knows afterglow (claude mcp get afterglow); this pairing is for {url}")
    else:
        subprocess.run(["claude", "mcp", "add", "--transport", "http", "afterglow", url,
                        "--header", f"Authorization: Bearer {token}"], check=True, capture_output=True)
        print(f"added afterglow ({url}) to Claude Code for {os.getcwd()}; start a new session to use it")
    return 0


def join_wifi(fd):
    """Ask for a network and its password on this computer, then join it on the headset.

    They travel to the headset's shell hex-encoded (so any characters survive) and are kept only in
    its RAM. The country decides which channels and transmit power the radio may use.
    """
    region = (locale.getlocale()[0] or "").partition("_")[2][:2].upper()
    ssid = input("Wi-Fi network name: ")
    password = getpass.getpass("Password (empty for an open network): ")
    country = input(f"Country code{f' [{region}]' if region else ''}: ").strip() or region
    if country:
        if not re.fullmatch(r"[A-Za-z]{2}", country):
            sys.exit(f"not a two-letter country code: {country}")
        status = run(fd, f"sh /afterglow/wifi.sh country {country}", 30)
        if status:
            return status
    return run(fd, f"sh /afterglow/wifi.sh join {ssid.encode().hex()} {password.encode().hex()}", 60)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--log", help="also append everything to this file")
    p.add_argument("--send", action="append", default=[], help="command to send once connected")
    p.add_argument("--until", help="stop once a line matches this regex")
    p.add_argument("--duration", type=float, help="stop after this many seconds connected")
    p.add_argument("--wait", type=float, default=120, help="seconds to wait for the port to appear")
    p.add_argument("--shell", action="store_true", help="use the shell port instead of the log")
    p.add_argument("--run", help="run one shell command, print its output, exit with its status")
    p.add_argument("--timeout", type=float, default=60, help="seconds to allow --run")
    p.add_argument("--wifi", action="store_true", help="join a Wi-Fi network (asks for it here)")
    p.add_argument("--push", metavar="LOCAL:REMOTE", help="copy a small file to the headset")
    p.add_argument("--pair", action="store_true", help="let this computer's Claude Code use afterglowd")
    args = p.parse_args()

    shell = args.shell or bool(args.run) or args.wifi or bool(args.push) or args.pair
    port = find_port(args.wait, shell)
    if not port:
        sys.exit(f"no afterglow {'shell' if shell else 'console'} appeared within {args.wait:.0f} s")
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
    tty.setraw(fd)
    if args.run:
        sys.exit(run(fd, args.run, args.timeout))
    if args.wifi:
        sys.exit(join_wifi(fd))
    if args.push:
        local, _, remote = args.push.partition(":")
        sys.exit(push(fd, local, remote))
    if args.pair:
        sys.exit(pair(fd))
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
