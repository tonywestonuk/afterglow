# afterglowd

afterglow's MCP server. Claude Code (or any MCP client) on a computer connects to it over Wi-Fi and
works on the headset directly: runs commands, reads and writes files, sees what the displays show,
reads the logs. It's the start of the "Claude-native" part of afterglow ([ROADMAP](../../ROADMAP.md)).

Go, standard library only; a static aarch64 binary of about 6 MB.

## Use it

afterglow starts it at boot, after Wi-Fi. The first time, with the computer connected over USB:

```sh
tools/console.py --wifi                                 # join a network (asks here)
tools/console.py --pair                                 # trust this computer; tell Claude Code
```

Its line on the headset's status screen says whether it's paired and which client last connected.

`--pair` adds the server to Claude Code for the project in the current directory (`claude mcp add`,
local scope), so start a new Claude Code session there and its tools appear as `afterglow`. Claude Code
asks before each tool call unless you allow them.

With afterglow storage ([docs/storage.md](../../docs/storage.md)) the headset keeps the token and its
Wi-Fi networks, so after a restart nothing is needed: it reconnects and Claude Code finds it again
(measured: answering 30 s after the kernel starts). Without storage, `--wifi` and `--pair` again.

## Tools

| Tool | Does |
|---|---|
| `run` | A shell command as root (BusyBox sh), with output, exit status and a timeout (default 30 s, at most 600) |
| `read_file` | Text as text, images as images, anything else as base64; offset and length for big files |
| `write_file` | Text or base64, replace or append, permissions; creates folders. Refuses anything under `/dev` (even through links) and anything that isn't a regular file |
| `list_dir` | Type, permissions, size, name, link targets |
| `status` | Uptime, load, memory, battery, SoC temperature, the Snapdragon's subsystems, addresses, Wi-Fi |
| `logs` | The kernel log (afterglow's and Android daemons' messages are there too) or a log file, last N lines, filtered by a regular expression |
| `screenshot` | Both eyes or one, as the wearer sees them, as PNG; 1/2 scale by default (1440×800). While the world runs it asks the world for its newest frame (the display flips 72 times a second, faster than it can be read): `eyes` (default) undistorted, `panels` warped for the lenses; `screen` reads the display itself |
| `/status` (not a tool) | POST `{"text": ...}` with the token: what Claude is doing, shown in the world and on the status screen; sent by the project's Claude Code hooks (`tools/claude_status_hook.py`) |
| `load_extension` | Compile C source for the 3D world on the headset and load it while the world runs; `edits` change the newest version instead of resending it; `placeholder` adds a road-works sign; or go back to an earlier `version` ([system/world](../world/README.md)) |
| `unload_extension`, `list_extensions` | Remove one (everything it did is undone); what's loaded, what failed and why, versions kept |
| `start_world`, `stop_world` | The world on the displays, or back to the status screen |
| `recentre` | Make the way the wearer faces "ahead" again (head tracking is turning only, so the heading drifts) |
| `generate_texture` | Has an image generator on the network paint a texture from a description (AUTOMATIC1111 web API, Draw Things' included; the address is remembered), keeps it for `texture_load`, returns a preview. Returns within 40 s with "still generating" if it's slow; calling again waits on the same job |
| `fetch_texture` | The same from an image URL (e.g. CC0 textures from Poly Haven or ambientCG) |

The server also gives the client short instructions (MCP `instructions`): what the device is, that
storage is RAM, that partitions must never be written, and how to work on the world: read the SDK
headers, put a placeholder up first when the real thing will take more than a minute to write, and
change extensions with edits.

For https, afterglowd uses Android's CA certificates (`/system/etc/security/cacerts`); the headset's
clock is set by NTP when Wi-Fi comes up (`udhcpc.sh`), since nothing sets it at boot.

Every tool call is logged to the kernel log (`afterglowd: run: …`), so the USB console shows what a
client does.

## Security

- **Pairing over USB.** Every request needs a bearer token. `tools/console.py --pair` makes it once
  on the computer (`~/.config/afterglow/token`, mode 0600) and puts it on the headset over the USB
  serial console, which only someone holding the headset and cable can use. The headset keeps it in
  `/run/afterglowd/token` (on `/storage` when it has afterglow storage); without it, afterglowd
  refuses everything (HTTP 503).
- **No browsers.** Requests with an `Origin` header are refused, so a web page can't reach the headset
  through someone's browser (including by DNS rebinding).
- **Plain HTTP on the local network.** The token is sent unencrypted, so anyone who can capture the
  Wi-Fi traffic could reuse it until the headset restarts. Use trusted networks; TLS is on the roadmap.
- **`run` is root.** That's what makes it useful for development. The guardrails are the token, Claude
  Code's own permission prompts, and the read-only mounts of the headset's partitions. Approval by
  the wearer, in the headset, is planned once afterglow can draw text.

## How it works

- **MCP over Streamable HTTP**, the subset a tools-only server needs: JSON-RPC messages POSTed to
  `http://afterglow.local:7070/mcp`, each answered with one JSON response. No event stream (GET gives
  405), no sessions. Protocol versions 2024-11-05 to 2025-11-25.
- **mDNS** (`mdns.go`): answers `afterglow.local` with the headset's IPv4 address on whichever
  interface has one, and states with an NSEC record that there's no IPv6 address. Without that a Mac
  waits 5 s for one on every connection (measured: 5.2 s per request before, 0.01 s after).
- **Screenshots** read the framebuffer page being shown (`yoffset`), undo the panels' 180° rotation,
  and average k×k pixel blocks. Each eye's image includes afterglow's correction for that panel's
  roll, so level lines look tilted by a degree or two. About 0.25 s on the headset.

Measured over Wi-Fi from a Mac (2026-10-19): about 10 ms for a small request, 6.2 s to upload 6 MB
with `write_file` (base64 inside JSON).

## Build and test

```sh
docker run --rm -v "$PWD":/work afterglow-userland /work/system/afterglowd/build.sh
```

Runs `go vet` and the tests (protocol, authorization, tools, `/dev` refusals, mDNS packets), then
writes `out/afterglowd/afterglowd`, which `system/build.sh` puts in the image. To try a new build
on a running headset without restarting it, upload it with `write_file` and restart it with `run`:

```sh
mv /tmp/afterglowd.new /bin/afterglowd && (sleep 1; kill $(pidof afterglowd)) > /dev/null 2>&1 &
```

`bringup.sh` runs it in a loop, so it's back a second later.
