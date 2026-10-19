#!/usr/bin/env python3
"""Show in the headset what Claude is doing: a Claude Code hook (.claude/settings.json).

Before each tool call it sends afterglowd's /status what Claude last wrote, from the session's
transcript, and what it's doing now, from the tool call itself (a command's description, the file it
edits, the extension it loads); afterglowd shows it on a panel in the world and on the status screen.
Claude Code saves a message to the transcript a little after its tool starts, so the words can be a
step or two behind; the step itself is always current. It also says "Reading your message" when a prompt arrives and "Done"
when Claude stops.

The hook itself returns at once and prints nothing: the sending is left to a detached copy of this
script, so a headset that's off or asleep never slows Claude down.

    tools/claude_status_hook.py          (as a hook: the event comes as JSON on stdin)
    tools/claude_status_hook.py --send TEXT
"""
import json
import os
import re
import subprocess
import sys
import urllib.request

HOST = os.environ.get("AFTERGLOW_HOST", "afterglow.local")
TOKEN_FILE = os.path.expanduser("~/.config/afterglow/token")


def send(text):
    with open(TOKEN_FILE) as f:
        token = f.read().strip()
    request = urllib.request.Request(
        f"http://{HOST}:7070/status", json.dumps({"text": text}).encode(),
        {"Content-Type": "application/json", "Authorization": "Bearer " + token})
    urllib.request.urlopen(request, timeout=3).read()


def plain(text):
    """Claude's markdown as a line of plain text: its first paragraph, without emphasis or code marks."""
    text = text.strip().split("\n\n")[0]
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)   # links
    text = re.sub(r"[*_`#>]+", "", text)
    return " ".join(text.split())[:240]


def latest_words(transcript):
    """The latest text Claude wrote in reply to the current prompt, if any."""
    try:
        with open(transcript, "rb") as f:
            f.seek(0, os.SEEK_END)
            f.seek(max(0, f.tell() - (4 << 20)))   # each request leaves a ~250 KB entry: look past a few
            lines = f.read().decode("utf-8", "replace").split("\n")[1:]
    except OSError:
        return ""
    for line in reversed(lines):
        try:
            entry = json.loads(line)
        except ValueError:
            continue
        message = entry.get("message") or {}
        content = message.get("content")
        if entry.get("type") == "user" and not (isinstance(content, list) and any(
                isinstance(b, dict) and b.get("type") == "tool_result" for b in content)):
            return ""   # back to the prompt: nothing said since
        if message.get("role") == "assistant" and isinstance(content, list):
            for block in content:
                if isinstance(block, dict) and block.get("type") == "text" and block.get("text", "").strip():
                    return plain(block["text"])
    return ""


def describe(tool, args):
    """What a tool call is doing, in a few words."""
    name = os.path.basename(str(args.get("file_path") or args.get("path") or ""))
    afterglow = {
        "load_extension": lambda: f"Loading {args.get('name', '')} into the world"
                                  + (" (a quick placeholder)" if args.get("placeholder") else ""),
        "unload_extension": lambda: f"Taking {args.get('name', '')} out of the world",
        "screenshot": lambda: "Looking at what you see",
        "generate_texture": lambda: f"Painting a texture: {args.get('name', '')}",
        "fetch_texture": lambda: f"Fetching a texture: {args.get('name', '')}",
        "run": lambda: "Running a command on the headset",
        "write_file": lambda: f"Writing {name} on the headset",
        "read_file": lambda: f"Reading {name} on the headset",
        "recentre": lambda: "Recentring your view",
        "start_world": lambda: "Starting the world",
        "stop_world": lambda: "Stopping the world",
    }
    if tool.startswith("mcp__afterglow__"):
        short = tool[len("mcp__afterglow__"):]
        return afterglow[short]() if short in afterglow else f"Using the headset: {short}"
    if tool == "Bash":
        return args.get("description") or "Running a command"
    if tool in ("Edit", "MultiEdit", "NotebookEdit"):
        return f"Editing {name}"
    if tool == "Write":
        return f"Writing {name}"
    if tool == "Read":
        return f"Reading {name}"
    if tool in ("Grep", "Glob"):
        return "Searching the code"
    if tool in ("WebSearch", "WebFetch"):
        return "Looking something up"
    if tool in ("Agent", "Task"):
        return "Asking a helper"
    if tool == "SendMessage":
        return "Talking to another Claude session"
    return f"Working ({tool})"


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--send":
        try:
            send(sys.argv[2])
        except Exception:
            pass   # the headset may be off: nothing to tell anyone
        return
    try:
        event = json.load(sys.stdin)
    except ValueError:
        return
    kind = event.get("hook_event_name")
    if kind == "PreToolUse":
        words = latest_words(event.get("transcript_path", ""))
        step = describe(event.get("tool_name", ""), event.get("tool_input") or {})
        text = f"{words[:150]}  \u25b8 {step}" if words else step
    elif kind == "UserPromptSubmit":
        text = "Reading your message…"
    elif kind == "Stop":
        text = "Done: over to you"
    else:
        return
    subprocess.Popen([sys.executable, os.path.abspath(__file__), "--send", text], start_new_session=True,
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
