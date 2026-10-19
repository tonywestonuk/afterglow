# Apps: live worlds that code is added to while they run

Design agreed 2026-10-19. Status: the world and its extensions work ([system/world](../system/world/README.md));
frames straight to the panels, lens distortion, head tracking and voice are still to come.

The goal, in one scene: a VR app starts with nothing but a floor of wireframe squares. The wearer says
"make the floor grass", and a few moments later the floor is grass. Behind that, Claude wrote a grass
extension, it was compiled, and it was linked into the running app, which never stopped.

## The model

- **The world** is a long-running process that owns the displays, the render loop, head pose and
  input, and all the **state**: a scene of entities (the floor, its tiles, later trees and lights)
  and their data.
- **Extensions** are small compiled modules that add behaviour or looks: a grass material, wind, a
  tree generator. Each has `on_load()`, `on_frame(dt)` and `on_unload()` (Arduino's `setup()` and
  `loop()`, per extension) and calls a versioned world API: meshes, shaders, materials, entities,
  input, time.
- **The loop**: Claude writes an extension → it's compiled (about a second) → afterglowd's
  `load_extension` tool hands it to the world → the world links it in between two frames → Claude
  checks with a screenshot and iterates. Most of the "few moments" is Claude writing code.
- **State lives in the world, not in extensions**, so an extension can be replaced without losing
  anything ("make the blades longer" changes only the code). Every version is kept on `/storage`:
  undo is loading the previous one, and a world rebuilds itself after a restart.

## Decisions

| Question | Decision | Why |
|---|---|---|
| How extensions are linked in | **WebAssembly**, run by WAMR (about 1 MB, ahead-of-time compiled to ARM code) | A bug in generated code (bad pointer, endless loop) traps that extension only: the world disables it, tells Claude why, and keeps rendering. Extensions can only do what the world API allows. Any language that compiles to WebAssembly works. Native code (TinyCC compiling into the process) was the alternative: faster to build, but any bug crashes the world |
| GPU | **Qualcomm's own driver** from the headset's Android partition | Quickest route to real 3D; closed, like the Wi-Fi firmware already used. The open alternative (mainline Linux + Mesa freedreno) is a long port |

## The GPU: proven 2026-10-19

`system/gpu/gles_probe.c` runs Qualcomm's OpenGL ES driver under afterglow and draws the wireframe
floor in stereo:

```
EGL 1.5: Qualcomm Inc., 1.5
GL: Qualcomm / Adreno (TM) 540 / OpenGL ES 3.2 V@0555.0 (Date:06/03/21) (4x multisampling)
144 stereo frames of 2880x1600 in 0.58 s: 4.02 ms each (249 fps); reading one back took 32.4 ms
```

How:

- It's built with the Android NDK (API level 29), so it runs on Android's own C library from the
  system partition, like Qualcomm's daemons.
- Android's EGL loader finds the vendor driver through system properties, which afterglow doesn't
  have, so the program `dlopen`s `/vendor/lib64/egl/libEGL_adreno.so` and `libGLESv2_adreno.so`
  directly and looks up each EGL and GL function by name.
- The driver loads the gralloc HAL, and without properties Android's HAL loader picks the generic
  `gralloc.default.so`, which the driver rejects ("Mismatch on build configuration"). `bringup.sh gpu`
  bind-mounts Qualcomm's `gralloc.msm8998.so` over it, in RAM; it also makes `/dev/ion`.
- Rendering is offscreen (an EGL pbuffer), so no window system is needed.

## Still to solve

| | |
|---|---|
| **Getting frames onto the panels** | The probe copies a finished frame to the framebuffer with `glReadPixels`: 32 ms, far too slow for 72 Hz (13.9 ms per frame). The GPU needs to render straight into buffers the display shows (the display driver's overlay with ION buffers, or the framebuffer's own memory imported as an EGLImage) |
| **Head tracking: position** | Turning is tracked (below); moving about needs the cameras (6DoF, open SLAM) |
| **Voice** | The microphones (the audio DSP already runs) and speech-to-text, on the Mac at first |
| **Instancing** | Many copies of one mesh (grass blades, puffs of smoke, crowds) in one draw, each with its own transform and colour |
| **Faster extensions** | They run in WAMR's fast interpreter (its JIT is x86-only). Ahead-of-time compilation with `wamrc`, which needs LLVM (already in the headset's compiler), would make heavy `on_frame()` code native |

## Built 2026-10-19

- **The world** (`system/world`): the render loop and scene on Qualcomm's driver, stereo views centred
  on each lens and turned by each panel's roll, the built-in wireframe floor, frames to the panels
  through double-buffered asynchronous readback (48 fps).
- **Extensions**: WAMR 2.4.5 embedded; the API in `sdk/afterglow.h`; replacing, undoing and
  recovering as described in [system/world](../system/world/README.md#how-extensions-behave).
- **Compiled on the headset**: clang 20 and the WASI C library in an Alpine root on `/storage`, run with
  `chroot`; 0.5 s per extension. Claude sends C source, never binaries, so any MCP client can do it.
- **afterglowd tools**: `load_extension`, `unload_extension`, `list_extensions`, `start_world`,
  `stop_world`; every version kept on storage.

## Head tracking, turning only (2026-10-19)

The world's `head.c` reads the IMU through SyncBoss ([docs/hardware.md](hardware.md#the-imu-syncboss)), a
thousand samples a second, and fuses them: the gyroscope turns the head, gravity keeps it level
(a complementary filter, as Mahony's), and the gyroscope's offset is learnt whenever the head is still.
A neck model swings the eyes about a point 7.5 cm below and 8 cm behind them. Each frame predicts the
head to when it will be seen, about two frames ahead, as the copy to the panels lags one behind. The
heading is recentred when the world starts and by afterglowd's `recentre`; extensions read the pose
with `head_pose()`.

Not yet: Meta's factory calibration of the IMU, the magnetometer to stop the heading drifting, and
lower latency (frames straight to the panels; reprojecting each refresh with the latest head pose).

## The lenses (2026-10-19)

Straight lines bent towards the sides, as through a fish-eye: the lenses magnify more away from their
centres, and nothing undid it. Now each eye is drawn undistorted into an eye buffer covering Meta's
field of view (52° out, 42° in, 53° up, 47° down), and `lens.c` warps the eye buffers onto the panels
with a 48 x 54 mesh per eye whose texture coordinates come from Meta's own lens model for the Quest 1
([docs/hardware.md](hardware.md#display-framebuffer)), separately for red, green and blue so colour
fringes cancel. The warp also turns the picture for the panels, which were drawn mirrored before.
The lens centres moved to Meta's (60 px further out than the splash logo suggested), so faraway
things no longer make the eyes converge. The warp is an extra pass: the village went from 35-40 to
about 29 frames drawn a second, still shown at every refresh.

## Changing one thing without restarting the rest (2026-10-19)

A village green with a dog, a cat, ducks and squirrels: every change to one animal restarted them all,
and they lost where they were. The world never restarts other extensions, but the animals were all in
one extension, because the API gave an extension no way to see another's objects. Now:

- `object_get_transform()` reads any object's transform, so the cat can watch the dog from its own
  extension: one extension per thing that changes on its own.
- `remember(key, data, size)` and `recall()`: the world's memory, named values that outlive an
  extension's versions and the world itself (`/storage/world/memory`, saved every 10 s and as the world
  stops). An extension keeps its state in `on_unload()` and gets it back in `on_load()`, so a new
  version carries on where the old one was. Keys are shared, so extensions can also tell each other
  things.
- When the world stops, every extension's `on_unload()` runs, so restarting the world (as a new world
  program needs) loses nothing an extension kept.

Tested: a counter carried from v1 to v2 (frame 106 on) and through a world restart (frame 230 on).

## Tilting, not yet (2026-10-19)

Turning and nodding are reprojected (the picture slides); tilting the head isn't, so a tilt shows the
double image again. Turning the picture on the CPU at every refresh doesn't fit in one (13.9 ms):

| Tried | Cost of a turned copy |
|---|---|
| Pixel by pixel from the readback buffer | 48-57 ms: the CPU sees that buffer uncached |
| Pixel by pixel from a cached copy (frames are now copied out once each) | 48 ms: 4-byte stores to the framebuffer, which is device memory, aren't combined |
| Rows built in cached memory, then copied whole; three threads | 15-28 ms |
| Rows as straight copies of pieces of the source rows (a small turn is a slide per row and a different source row every 1/angle pixels) | 27-29 ms: each piece comes from a different row, and the memory system can't follow dozens of interleaved rows (prefetching didn't help) |

So the code is there but off (`TILT_REPROJECTION`). The way forward is the GPU: Oculus's timewarp
redraws the lens warp at every refresh from the newest eye buffers, turned for the head in all three
axes, on a high-priority context; that also makes turning and nodding exact at the edges (sliding
the warped picture isn't). `worldctl test-tilt DEGREES` forces a tilt, to measure without a wearer.

## Frame rate (2026-10-19)

With tracking, a slow frame rate shows as the world lagging the head. A village scene ran at 23-28
fps, unevenly; measured part by part, then fixed:

| Cause | Fix | Result |
|---|---|---|
| The CPU's copy to the framebuffer reversed every row, pixel by pixel, into uncached memory: ~20 ms | Draw the frame mirrored (x negated in the projection, eyes' halves swapped), so the copy is a straight `memcpy` | 2.7-5.5 ms |
| The GPU stayed at 257 MHz: its governor (`msm-adreno-tz`) saw it idle while the CPU worked | The world holds `min_pwrlevel` at 0 (710 MHz) while it runs, gives it back when it stops or if a SoC sensor passes 70 °C | Drawing the village: 17.6 ms |
| A frame's commands reached the GPU only when the CPU next waited, so drawing, copying and `FBIOPAN_DISPLAY` (which blocks until the next refresh, ~12 ms) ran one after another | `glFlush()` once the frame is submitted, so the GPU draws while the previous frame is copied and shown | GPU wait 23 ms → 5.5 ms |
| 4x multisampled pbuffer: four samples a pixel to memory, resolved on readback | A framebuffer object with `EXT_multisampled_render_to_texture`: multisampling stays in tile memory | (no measurable change here) |

That gave a steady 36 fps: every other refresh, as drawing the village takes the GPU 17.6 ms and a
refresh is 13.9 ms. But showing each frame twice on a low-persistence display gives a double image
when the head turns (the eye moves on between the two flashes: two half-bright copies, "transparent"),
and the prediction needed (about 64 ms) made it jitter.

**Reprojection** (Oculus's timewarp, in its simplest form): drawing and showing now run on separate
threads. The main thread draws frames as fast as the scene allows (the village: 34-40 a second); the
display thread shows the newest finished frame at every refresh, 72 a second, slid by the head's turn
since that frame was drawn (yaw sideways, pitch up and down, in the copy it makes anyway; roll is
ignored). Prediction is down to one refresh (19 ms). The display thread runs on a fast core (7) at a
real-time priority: left to the scheduler it sometimes landed on a slow core, the copy took a whole
refresh (14 ms instead of 4-5), and a refresh was missed about once a second, a visible hitch. Pinned,
none in minutes of running; each miss is logged with its timings. Moving things in a scene still update at the
drawing rate. A scene needs under about 11 ms of GPU a frame to be drawn at 72; the world's log line
gives frames drawn and shown a second and the GPU's busy share every 10 s.

## Something on screen in seconds (2026-10-19)

A complex scene took minutes to appear. Compiling took about a second; nearly all of the time was
Claude writing 300-700 lines of C, resent in full for every tweak, with the same helpers (vectors,
boxes, lathes, a pixel font) written again in every extension. So:

- **Placeholders.** For anything that will take more than a minute to write, Claude first loads a
  stand-in under the same name, `placeholder: true`: a few `shape()` calls, roughly where and how big
  the real thing will be, in its colours. The world puts a road-works sign beside it (a red triangle
  with a man digging, MEN AT WORK, an amber lamp flashing). The real version, loaded under the same
  name, replaces both. Measured: a placeholder cottage on screen 2 s after it was sent.
- **Edits.** `load_extension` takes `edits` (old and new text) against the newest version, so "make
  the blades longer" costs a line.
- **`shapes.h`.** The helpers every extension was writing: vectors, poses, a mesh builder (boxes,
  wedges, spheres, cylinders, cones, lathes, quads), `shape()` and `label()`, random numbers, noise.
- **A standard shader** (lit by a sun and the sky, colour x vertex colour x texture, emission), so most
  things need no GLSL; **blending** (alpha and additive, drawn farthest first).

## Textures (2026-10-19)

Three ways in, all ending as a texture an extension uses with `texture_load("name")` (or makes itself):

| Way | How | For |
|---|---|---|
| Painted | `texture_create(pixels)`: the extension computes them (noise, bricks, stripes) | Instant; patterns and procedural materials |
| Generated | afterglowd's `generate_texture`: the headset asks an image generator on the network (Stable Diffusion and the like, through the AUTOMATIC1111 web API that AUTOMATIC1111, Forge, SD.Next and Draw Things' HTTP server offer), with `tiling` for seamless ones | Anything describable: "mossy cobblestones" |
| Fetched | afterglowd's `fetch_texture`: any PNG or JPEG URL; CC0 libraries (Poly Haven, ambientCG) are made for this | Real-world materials, photographed |

The image goes from the generator or the web straight to the headset; Claude only sees a small
preview (2x2 for seamless ones, so a seam would show). Images that should repeat but don't are
blended at their edges. Text becomes a texture too (`texture_text`, `label()`), in afterglow's font.
Proven: fetching (Poly Haven over https) and the generator round trip against a stand-in speaking the
AUTOMATIC1111 API. Draw Things (2026-10-19, on the Mac): its HTTP API server answers
`/sdapi/v1/txt2img` with AUTOMATIC1111's reply, but refuses fields it doesn't know, `tiling` among
them, so afterglowd asks again without them (seamless then comes from the edge blending); it
listens only on 127.0.0.1 until set otherwise. A 1024x1024 texture with PixArt Sigma at 20 steps
took 120 s on the Mac: placeholders matter. DiffusionBee and Mochi Diffusion have no API.

Lessons from texturing a supermarket with it (2026-10-19):
- A model makes garbage away from the size it was made for (PixArt Sigma at 512: glitchy stripes,
  straight from Draw Things). afterglowd never sends a size; `size` only shrinks the result.
- Generating outlasts MCP clients' patience (about a minute), and a client that gives up and asks
  again queues duplicates. A call now waits at most 40 s, then says "still generating"; calling again
  with the same name and prompt waits on the same job.
- Diffusion models don't follow "an N x M grid of designs": the cells come out in other counts and
  shapes. Measure the cells in the result, or generate one design per image.
