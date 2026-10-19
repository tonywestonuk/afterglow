# The world

A long-running 3D scene on the GPU that grows by extensions: C compiled to WebAssembly on the headset
and linked in while it runs ([docs/apps.md](../../docs/apps.md) has the design). Claude drives it
through afterglowd's tools; the extension API is [`sdk/afterglow.h`](sdk/afterglow.h).

| File | What |
|---|---|
| `world.c` | Render loop, scene (meshes, shaders, textures, objects, with who changed what), the standard shader, blending, stereo views centred on the lenses and turned by each panel's roll, frames to the panels, control socket |
| `ext.c` | Extensions in WAMR's fast interpreter: the API as WebAssembly imports, an instruction budget per call, unloading and restoring on failure |
| `lens.c` | The lenses: Meta's lens model for the Quest 1, a warp mesh per eye (per colour) from the eye buffers to the panels |
| `head.c` | Head tracking, turning only: the IMU through SyncBoss, fused into the head's orientation, a neck model, prediction |
| `capture.c` | Frames for afterglowd's video stream: scaled down on the GPU, read back without waiting, into a shared file of two slots |
| `memory.c` | The world's memory: named values (`remember`, `recall`) that outlive extensions' versions and the world, kept on storage |
| `sign.c` | What Claude is doing: the "Claude at work" sign, with a message board of Claude's status, beside placeholders and while Claude works |
| `paint.c` | Pictures on the CPU: images from files (stb_image), text (stb_truetype, from distance fields so the light font can be made bolder), the sign's face |
| `gles.c` | Qualcomm's OpenGL ES driver, loaded by name (no Android properties) |
| `sdk/afterglow.h` | The API extensions are written against (on the headset in `/afterglow/sdk/`) |
| `sdk/shapes.h` | Building blocks: vectors, transforms, a mesh builder (boxes, wedges, spheres, cylinders, cones, lathes, quads), `shape()` for a whole object in a line, `label()` for text, noise |
| `toolchain.sh` | Packs the compiler for the headset: clang, lld and the WASI C library in an Alpine root |
| `build.sh`, `CMakeLists.txt` | Builds `out/world/world` with the Android NDK and WAMR 2.4.5 |

## Build

On the Mac, with the NDK in `tools/android-ndk-dl/` ([system/gpu](../gpu/README.md)):

```sh
system/world/build.sh                     # out/world/world, then system/build.sh puts it in the image
```

## The compiler on the headset

Once per headset, on afterglow's storage (333 MB of its 2.4 GB):

```sh
docker run --rm --platform linux/arm64 -v "$PWD":/work alpine:3.22 /work/system/world/toolchain.sh
python3 tools/push_file.py out/world/toolchain.tar.gz --extract /storage/toolchain   # about 30 s over Wi-Fi
```

`/afterglow/wasm-cc.sh` then compiles an extension inside that root with `chroot`, in about half a second.

## How extensions behave

- `load_extension` (afterglowd) keeps the source as a new version under `/storage/world/extensions/NAME/`,
  compiles it, starts the world if needed and loads it. Compiler errors come back with file and line;
  the world is left as it was.
- A new version replaces the old one: the old one's `on_unload()` runs, everything it created is
  deleted and everything it changed on other objects (the world's "floor", say) is put back; then the
  new one's `on_load()` runs. If that fails, the previous version is loaded again.
- A trap (a bad pointer, `__builtin_trap()`) or running past the instruction budget (2 billion
  instructions for `on_load()`, 100 million for each `on_frame()`) stops that extension and undoes it;
  `list_extensions` says why. A shader that doesn't compile doesn't stop anything, but the GLSL
  compiler's messages come back with the load.
- The loaded extensions and their versions are in `/storage/world/active.json`, so a restarted world
  comes back as it was. `version` in `load_extension` goes back to an earlier version.
- `edits` in `load_extension` change the newest version's source (each old text must appear exactly
  once) and load the result as a new version: a tweak costs a line, not the whole file again.
- **Placeholders.** `placeholder: true` marks a version as a stand-in: after its `on_load()` the world
  stands a "Claude at work" sign (red triangle, a figure digging, CLAUDE AT WORK, an orange asterisk spinning) on the
  floor at the front right of everything it made, turned to the wearer, and bigger the farther away
  it is. The sign belongs to that version, so the real one, loaded under the same name, replaces both.
  It's how Claude shows something within seconds while it writes something that takes minutes.
- **Textures** are kept in `/storage/world/textures` (`$AFTERGLOW_WORLD_DIR/textures`) as PNG or JPEG;
  `texture_load("name")` reads them. afterglowd's `generate_texture` and `fetch_texture` put them there.
- The control socket is `/run/afterglow/world.sock`: `load NAME PATH [placeholder]`, `unload NAME`, `list`,
  `recentre`, `quit`.
  While the world runs it owns the display (`/run/afterglow/display-owner`); `/init`'s status screen
  returns when it stops.

Each 10 s the world logs frames drawn and shown a second (it shows one at every refresh, reprojected
for the head's turn, however fast it draws), and how busy the GPU is: a scene wants under about 11 ms
of GPU a frame to be drawn at 72
([docs/apps.md](../../docs/apps.md#frame-rate-2026-10-19)).

Placeholders, measured 2026-10-19: a four-shape cottage loaded with its sign 2 s after it was sent
(compile 0.9 s); its textured replacement, fixed by a one-line edit, 1.6 s. A 1024² CC0 texture
fetched from Poly Haven in 1.1 s. A full-screen textured floor brings the world to 36 fps: the GPU
sits at its lowest clock (257 MHz), and filling 2880×1600 four times over shows.

Measured 2026-10-19: compile 0.5 s, load 11 ms, MCP round trip 0.3 s; the wireframe floor at 48 fps,
150,000 animated triangles (generated in 97 ms by the interpreter) at 33 fps. Nearly all of each frame
is the copy to the screen, which goes when the GPU can render straight into what the display shows.
