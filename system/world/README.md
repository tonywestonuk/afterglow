# The world

A long-running 3D scene on the GPU that grows by extensions: C compiled to WebAssembly on the headset
and linked in while it runs ([docs/apps.md](../../docs/apps.md) has the design). Claude drives it
through afterglowd's tools; the extension API is [`sdk/afterglow.h`](sdk/afterglow.h).

| File | What |
|---|---|
| `world.c` | Render loop, scene (meshes, shaders, objects, with who changed what), stereo views centred on the lenses and turned by each panel's roll, frames to the panels, control socket |
| `ext.c` | Extensions in WAMR's fast interpreter: the API as WebAssembly imports, an instruction budget per call, unloading and restoring on failure |
| `gles.c` | Qualcomm's OpenGL ES driver, loaded by name (no Android properties) |
| `sdk/afterglow.h` | The API extensions are written against (on the headset at `/afterglow/sdk/afterglow.h`) |
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
- The control socket is `/run/afterglow/world.sock`: `load NAME PATH`, `unload NAME`, `list`, `quit`.
  While the world runs it owns the display (`/run/afterglow/display-owner`); `/init`'s status screen
  returns when it stops.

Measured 2026-10-19: compile 0.5 s, load 11 ms, MCP round trip 0.3 s; the wireframe floor at 48 fps,
150,000 animated triangles (generated in 97 ms by the interpreter) at 33 fps. Nearly all of each frame
is the copy to the screen, which goes when the GPU can render straight into what the display shows.
