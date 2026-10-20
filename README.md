# afterglow

**Put on the headset and say where you want to be.**

> You can't use afterglow to pirate games. In afterglow, you won't need to.

An open operating system for the Meta Quest 1, built so that Claude can make worlds on it while you
stand in them. Not affiliated with, or endorsed by, Meta.

## The idea

"Put me on a village green." "Put me on the summit of Everest." "Put me on a space dock under attack by
aliens." A few moments later you're standing there, and the place keeps changing as you talk: add a dog;
make it chase the squirrels; make it night.

If you know Star Trek's holodeck, that's how afterglow is used. There's no store of apps to download:
the place becomes whatever you ask for, and it's kept, so you can go back.

None of it is a pre-made asset, or video streamed from a server. Every place is a set of small programs
that Claude writes, compiled on the headset and linked into a world that never stops running. The
village's spaniel is real code: it wanders, chases squirrels up trees, jumps in the pond and shakes
itself dry. The summit of Everest is built from real elevation data. On the space dock, the turrets
fire back at the saucers. Claude is needed to make and change a place, but the place runs on the
headset itself.

Why the Quest 1? Meta has ended its support, and most of them sit in drawers. Since August 2026 its
bootloader can be unlocked, and underneath is a capable computer: a Snapdragon 835, 4 GB of RAM, two
1440×1600 OLED displays at 72 Hz, four tracking cameras, motion sensors and controllers. *Afterglow* is
the light left after sunset: this is the Quest 1's life after Meta. It's Linux underneath, and
everything above that is ours, and open.

## How a place is made

A place isn't one program. It's a **world** that keeps running, and **extensions**, one for each
thing in it, which Claude writes, compiles and swaps in while the world runs.

```
 Mac or PC                                    Quest 1
┌────────────────────────┐                    ┌─────────────────────────────────────────┐
│ Claude Code            │  load_extension    │ afterglowd (MCP server)                 │
│ "make the cat a tabby" │ ─── Wi-Fi, MCP ──► │   keeps it as extensions/cat/v3.c       │
│  writes cat.c (or an   │                    │   compiles it on the headset: v3.wasm   │
│  edit to it)           │                    ├─────────────────────────────────────────┤
│                        │  screenshot        │ the world (always running, 72 Hz)       │
│  checks the result   ◄─┤─────────────────── │   between two frames: cat v2 out, v3 in │
└────────────────────────┘                    │   the rest carry on untouched           │
                                              └─────────────────────────────────────────┘
```

**The world** owns the displays, head tracking, the render loop and the scene: meshes, shaders,
textures and objects. It starts as an empty floor, and it never restarts when the place changes.

**An extension** is a C file with up to three functions. A complete one:

```c
#include "shapes.h"

static struct { float height, speed; } ball = { 1.5f, 0 };
static int object;

void on_load(void)
{
    keep("ball", &ball, sizeof ball);   /* a new version carries on mid-bounce */
    object = shape("ball", SPHERE, v3(0, ball.height, -2), v3(0.3f, 0.3f, 0.3f), 0xff7a59);
}

void on_frame(float dt)
{
    ball.speed -= 9.8f * dt;
    ball.height += ball.speed * dt;
    if (ball.height < 0.15f)
        ball.height = 0.15f, ball.speed = -ball.speed * 0.9f;
    place(object, v3(0, ball.height, -2), 0, v3(1, 1, 1));
}
```

(`on_unload()` is the third, for anything to do before a version is replaced.) Claude sends the source
with afterglowd's `load_extension` tool. afterglowd keeps it as the next version, compiles it to
WebAssembly with clang (installed on afterglow's storage, in about a second), and hands it to the
world, which links it in between two frames.

### One extension per thing

The village green is six extensions: `village`, `lawn`, `dog`, `cat`, `squirrels` and `ducks`. "Make
the cat a tabby" recompiles only `cat`. The world runs the old version's `on_unload()`, deletes
everything it made, puts back anything it changed on other objects, and runs the new version's
`on_load()`. The dog doesn't notice: it carries on chasing squirrels.

### State carries on

`keep()` ties a variable to the world's memory, which is kept on storage and saved every 10 s. A new
version starts with the old one's state (the cat is where it was, mid-stride), and so does a world that
has restarted. Extensions can still see each other:

- `object_get_transform()` reads any object, so the cat can watch where the dog is.
- `remember()` and `recall()` pass news by name ("green.dog": chasing a squirrel).
- Headers in `/storage/world/include` share code. The space dock's layout, for example, is shared by
  the station, the battle and the dog on its deck.

### Mistakes stay small

- Extensions run sandboxed, in WebAssembly ([WAMR](https://github.com/bytecodealliance/wasm-micro-runtime)),
  with a budget of instructions for each call. A bad pointer or an endless loop stops and undoes that one
  extension; the world keeps drawing and tells Claude why.
- Compiler errors come back with file and line, and the world is left as it was. A shader that doesn't
  compile returns the GLSL compiler's messages.
- Every version is kept, so going back means loading an earlier one.

### Something to see in seconds

A detailed place takes Claude minutes to write. So the first thing loaded is a **placeholder**: a few
boxes and spheres where the real things will be. Beside it stands a "Claude at work" sign: a road-works
triangle with an orange asterisk spinning on top, and a message board showing what Claude is doing
right now (the project's Claude Code hooks pass on each step). The real version then replaces the
placeholder under the same name.

After that, changes go as **edits** (old text to new text), not whole files. `shapes.h` and a lit
standard shader mean most things need no hand-made meshes or GLSL.

### Textures

- **Generated** by an image model on your network (`generate_texture`): Draw Things, or anything
  speaking the AUTOMATIC1111 API.
- **Fetched** from CC0 libraries such as Poly Haven (`fetch_texture`).
- **Painted** by the extension itself.

Images go straight to the headset; Claude sees a small preview.

### Seeing what the wearer sees

- `screenshot` gives Claude each eye, undistorted.
- `start_capture` streams the wearer's view as video to VLC on the computer (Motion JPEG at a private
  URL), or to ffmpeg for a recording.

### Under the world

- Qualcomm's OpenGL ES 3.2 driver on the Adreno 540, loaded with no Android running.
- Each eye is drawn into a buffer covering Meta's field of view, then warped through Meta's lens model
  for the Quest 1, separately for each colour so the fringes cancel.
- A real-time thread shows a frame at every 72 Hz refresh, slid for the head's latest turn
  (reprojection), however fast the place itself draws.
- Head tracking comes from the IMU behind Meta's SyncBoss microcontroller, read a thousand times a
  second and fused, with a neck model and prediction.

The design and its measurements are in [docs/apps.md](docs/apps.md); the extension API is
[`sdk/afterglow.h`](system/world/sdk/afterglow.h), with building blocks in
[`shapes.h`](system/world/sdk/shapes.h).

## Where it's going

- **Places you can go back to:** saved by name, and chosen from inside the headset. That includes a
  console you can call up from anywhere, as the holodeck has its arch.
- **Talking to it in the headset**, rather than typing to Claude on the computer.
- **Controllers, walking about** (6DoF tracking from the cameras), and **sound**.
- **An installer**, so you don't need a developer's setup.

See [ROADMAP.md](ROADMAP.md).

## Status

**It works, but there's nothing to install yet.** afterglow runs from RAM (`fastboot boot`) and keeps
its files on the spare slot's system partition. The Android slot isn't touched, and a normal restart
goes back to Android. Building it needs a Mac or Linux PC with Docker and the Android NDK.

Done so far:
- Meta's Quest 1 kernel, **built from source** ([kernel/build](kernel/build/README.md)), booting
  afterglow's own `/init` with no Android: a 3D boot animation, buttons, and a USB serial console with
  the kernel log and a shell ([system](system/README.md)).
- **Wi-Fi with no Android running**: the Snapdragon's DSPs and GPU started with their firmware
  ([how](docs/hardware.md#subsystems-and-wi-fi-without-android)). It comes up by itself about 30 s after
  boot.
- **afterglowd, an MCP server on the headset**, paired over USB: Claude Code on a Mac runs commands,
  edits files, reads logs and sees the displays over Wi-Fi ([system/afterglowd](system/afterglowd/README.md)).
- **Its own storage**, remembering Wi-Fi and pairing across restarts ([docs/storage.md](docs/storage.md)).
- **The world**: GPU rendering on Qualcomm's driver, lens correction, reprojection at 72 Hz, and
  extensions compiled on the headset and swapped in while it runs, each keeping its state
  ([system/world](system/world/README.md)).
- **Head tracking (turning)** from the IMU, read directly ([how](docs/hardware.md#the-imu-syncboss)).
- Placeholders with the "Claude at work" sign, Claude's live status shown in the headset, textures from
  an image generator or the web, video capture to VLC, and switching off with a double press of Power.
- Places built so far: a village green with its animals, the summit of Everest, a space dock under
  attack, a supermarket aisle, and a rocket on a launch pad at night.

Details: [docs/hardware.md](docs/hardware.md), [docs/rooting-and-backup.md](docs/rooting-and-backup.md).

## Requirements

- A Meta Quest 1 on firmware `49845030443200410` (the last release) with an **unlocked bootloader**,
  e.g. via [QuestStack](https://github.com/starseed12345/QuestStack) or the
  [web unlocker](https://quest1-unlock.skystate.ch/). Unlocking factory-resets the headset and
  permanently ends access to Meta's store and account services on it.
- A Mac or Linux PC with a **current** [Android platform-tools](https://developer.android.com/tools/releases/platform-tools)
  (old `fastboot` builds stall on large transfers).
- To make places: [Claude Code](https://claude.com/claude-code) or another MCP client. Optionally, an
  image generator with an AUTOMATIC1111-style API (e.g. Draw Things) for textures.

## Safety rules

1. **Back up every partition first** ([how](docs/rooting-and-backup.md)). `persist`, `private` and `vision`
   are likely unique to each headset (calibration, keys) and cannot be downloaded anywhere.
2. **Test with `fastboot boot`**, which runs an image from RAM. A normal restart returns to stock.
3. **Never run `fastboot set_active`** without knowing what each slot contains. Unlock tools rewrite the
   inactive slot with an old, mismatched firmware chain.
4. Nothing from Meta (firmware, backups, images) is ever committed to this repository.

## Repository layout

| Path | What |
|---|---|
| `system/world/` | The world: rendering, lenses, head tracking, extensions (WAMR), the sign, capture; the extension SDK in `sdk/` |
| `system/afterglowd/` | The MCP server on the headset: shell, files, screenshots, extensions, textures, video |
| `system/` | afterglow's `/init` (boot animation, display, buttons, calibration), subsystem and Wi-Fi bring-up, third-party userspace build |
| `kernel/configs/` | The exact config of Meta's running Quest 1 kernel (from `/proc/config.gz`) |
| `kernel/build/` | Reproducible kernel build (Docker, Meta's GCC 4.9) and boot-image packaging |
| `kernel/patches/` | afterglow's kernel changes (GPL-2.0) |
| `brand/` | Logo, the 3D boot animation scene (Blender Python), fonts |
| `tools/backup_partitions.sh` | Back up all partitions (except `userdata`) to your computer, sha256-verified |
| `tools/root-overlay/` | Magisk `overlay.d` files that pre-approve root for the adb shell (Quest can't show Magisk's prompt) |
| `tools/console.py`, `tools/push_file.py` | USB console, shell, Wi-Fi setup and pairing; large files to the headset over Wi-Fi |
| `tools/claude_status_hook.py`, `.claude/` | Claude Code hooks that show Claude's progress in the headset |
| `tools/pack_anim.py`, `tools/make_calibration_frames.py` | Pack rendered frames for the headset; calibration test patterns |
| `docs/` | Design, hardware notes and procedures |

## Licence

afterglow's own code is [MIT](LICENSE). Changes to the Linux kernel are GPL-2.0, like the kernel itself.
Boot images also include third-party code, built from upstream releases:
- From `system/third_party/Dockerfile`: BusyBox (GPL-2.0), wpa_supplicant (BSD-3-Clause) and e2fsprogs
  (GPL-2.0), statically linked with musl (MIT), libnl (LGPL-2.1) and OpenSSL (Apache-2.0).
- In `/init`: zstd's decoder (BSD-3-Clause), stb_truetype (public domain / MIT) and the Montserrat
  typeface (SIL OFL).
- In the world: WAMR (Apache-2.0 with LLVM exception) and stb_image (public domain / MIT).
- afterglowd is built with Go (BSD-3-Clause).

Their licences are in the image under `/usr/share/licenses`.
Meta's firmware is not ours to distribute; tools extract it from each user's own headset or from Meta's
official update files.

## Credits

- [QuestStack](https://github.com/starseed12345/QuestStack) and darknight1050's
  [quest-bootloader-unlocker](https://github.com/darknight1050/quest-bootloader-unlocker), which made this possible.
- [WAMR](https://github.com/bytecodealliance/wasm-micro-runtime) and
  [stb](https://github.com/nothings/stb), inside the world.
- [Magisk](https://github.com/topjohnwu/Magisk), [postmarketOS](https://postmarketos.org) and
  [Monado](https://monado.freedesktop.org), whose work we build on and learn from.
