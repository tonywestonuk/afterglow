# Apps: live worlds that code is added to while they run

Design agreed 2026-10-19. Status: the GPU part is proven (below); the rest is to be built.

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
| **Lens centring and distortion** | Each eye's projection must be centred on its lens (852,865 and 2028,865 on screen), and the image pre-warped for the lens; the roll correction (already per pixel on the CPU) moves into the same GPU pass |
| **Head tracking** | The motion sensor is behind Meta's SyncBoss microcontroller; Meta's kernel has its driver, the data format needs working out. Without it the floor turns with your head |
| **The world and extensions** | WAMR built for Android's C library; the world API; `load_extension` and friends in afterglowd |
| **Voice** | The microphones (the audio DSP already runs) and speech-to-text, on the Mac at first |

Order: the world and extensions can be built now on the GPU (shown flat, then with lens centring),
while the display path and head tracking are worked out; voice last.
