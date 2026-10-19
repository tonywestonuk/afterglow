#!/usr/bin/env python3
"""Copy a large file to the headset over Wi-Fi: serves it from this computer for a moment and has
afterglowd fetch it (needs a paired afterglowd: tools/console.py --pair). Standard library only.

Usage: push_file.py FILE DEST              save it on the headset as DEST
       push_file.py FILE.tar.gz --extract DIR    unpack it into DIR on the headset
"""
import argparse
import functools
import http.server
import json
import os
import socket
import sys
import threading
import urllib.request

URL = "http://afterglow.local:7070/mcp"


def run(command, timeout):
    """Run a shell command on the headset through afterglowd's run tool; returns (ok, output)."""
    with open(os.path.expanduser("~/.config/afterglow/token")) as f:
        token = f.read().strip()
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                       "params": {"name": "run", "arguments": {"command": command, "timeout_seconds": timeout}}})
    req = urllib.request.Request(URL, body.encode(), {"Content-Type": "application/json",
                                                      "Authorization": "Bearer " + token})
    with urllib.request.urlopen(req, timeout=timeout + 30) as r:
        result = json.load(r)["result"]
    text = result["content"][0]["text"]
    return not result.get("isError") and "--- exit status 0" in text, text


def local_address():
    """This computer's address on the network the headset is on (the route to afterglow.local)."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect((socket.gethostbyname("afterglow.local"), 7070))
        return s.getsockname()[0]


def main():
    p = argparse.ArgumentParser()
    p.add_argument("file")
    p.add_argument("dest", nargs="?")
    p.add_argument("--extract", metavar="DIR", help="unpack a .tar.gz into DIR")
    args = p.parse_args()
    if not args.dest and not args.extract:
        sys.exit("give a destination, or --extract DIR")

    folder, name = os.path.split(os.path.abspath(args.file))

    class OneFile(http.server.SimpleHTTPRequestHandler):
        """Serves this one file and nothing else in its folder"""
        def do_GET(self):
            if self.path != "/" + name:
                self.send_error(404)
            else:
                super().do_GET()

        def log_message(self, *a):
            pass

    server = http.server.ThreadingHTTPServer((local_address(), 0), functools.partial(OneFile, directory=folder))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://{server.server_address[0]}:{server.server_address[1]}/{name}"
    if args.extract:
        command = f"mkdir -p '{args.extract}' && wget -q -O - '{url}' | tar -xzf - -C '{args.extract}'"
    else:
        command = f"mkdir -p \"$(dirname '{args.dest}')\" && wget -q -O '{args.dest}' '{url}'"
    ok, text = run(command, 600)
    server.shutdown()
    print(text)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
