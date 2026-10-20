# Roadmap

afterglow is built in phases. Each phase ends with something that works and can be demonstrated.

## Architecture (target)

```
 Mac / PC                                   Quest 1 (afterglow)
┌─────────────────────┐   Wi-Fi (USB to     ┌────────────────────────────────┐
│ Claude Code         │   pair)             │ afterglowd (MCP server)        │
│ (or any MCP client) │ ◄─────────────────► │  ├─ extensions: compile, load  │
└─────────────────────┘   MCP over HTTP     │  ├─ screenshot, video capture  │
          │                                 │  ├─ textures (generate, fetch) │
          ▼                                 │  └─ shell, files, logs         │
   Claude (cloud)                           ├────────────────────────────────┤
                                            │ The world: one running scene;  │
                                            │ extensions (C → WebAssembly)   │
                                            │ swapped in while it runs       │
                                            ├────────────────────────────────┤
                                            │ Linux (start: Meta's 4.4       │
                                            │ kernel; later: mainline)       │
                                            └────────────────────────────────┘
```

Design principles:
1. **A stable app API**, versioned and separate from the kernel, so apps survive OS updates.
2. **Apps are files**, loaded at runtime — never reflashed.
3. **Apps are isolated** (no root, own memory), so a crash returns to the launcher.
4. **USB comes first** (console, networking, app upload), Wi-Fi later.
5. **The OS API is the MCP tool set**: the launcher and Claude use the same functions.
6. **Risky actions are approved by the person wearing the headset**; Claude pairs with a code.
7. **Android stays in the other slot** as a fallback.

## Phase 0 — Research (in progress)

- [x] Confirm firmware, slots, partition layout and display (2880×1600 @ 72 Hz)
- [x] Prove `fastboot boot` runs unsigned images from RAM
- [x] Temporary root without flashing ([procedure](docs/rooting-and-backup.md))
- [x] Full checksum-verified partition backup
- [x] Identify board and panel (`vs1 pvt b (SDC)`) and matching kernel source files
- [x] Capture the running kernel's exact config
- [x] Build Meta's Quest 1 kernel from source and boot it with `fastboot boot` ([how](kernel/build/README.md))
- [ ] Find out whether root on stock Android can read the tracking cameras and motion sensor
- [ ] Script the root-image build (`tools/make_root_image.sh`)

## Phase 1 — Our own kernel on screen (in progress)

- [x] Boot our kernel with our own `/init` from RAM ([system/](system/README.md))
- [x] Draw on both displays (MDSS framebuffer, rotated 180°, double-buffered)
- [x] 3D stereo boot animation ([brand/](brand/README.md))
- [x] Read the buttons; interactive display roll calibration
- [x] USB serial console: live kernel log and commands ([tools/console.py](tools/console.py))
- [x] Development shell over USB (BusyBox); hardware survey ([docs/hardware.md](docs/hardware.md))
- [ ] Text console on the displays (the status screen renders text already)
- [x] Start the Snapdragon's subsystems and load their firmware (adsp, modem, a540_zap): the key to
      Wi-Fi, audio and the GPU ([how](docs/hardware.md#subsystems-and-wi-fi-without-android))
- [x] Wi-Fi: scan, join (WPA2), DHCP, DNS, with upstream `wpa_supplicant`
- [x] Keep settings (Wi-Fi networks, pairing) across restarts: `/storage` on the spare slot's system
      partition ([docs/storage.md](docs/storage.md))
- [x] Start afterglow's services at boot, then a status screen instead of the looping animation
- [ ] Hardware RNG driver (the random pool takes ~20 s to seed, which holds up Wi-Fi)
- [ ] Replace Qualcomm's Android helper programs with open ones (linux-msm's `pd-mapper`,
      `tqftpserv`, `rmtfs`), so afterglow doesn't need the `system` partition. They use QRTR sockets;
      Meta's kernel has the older IPC Router (`CONFIG_IPC_ROUTER`), so they'd need porting
- [ ] Get frames from the tracking cameras (`/dev/video*` are already there)
- [ ] Boot a minimal Linux userspace (postmarketOS-style "downstream" port on Meta's kernel)
- [ ] USB networking (NCM is already enabled in Meta's kernel config)
- [ ] SSH / shell over USB
- [ ] Read Meta's factory display calibration correctly (roll units, decentre)

## Phase 2 — The app platform: live worlds ([design](docs/apps.md))

An app is a running world that code is added to while it runs: "make the floor grass", and Claude's
grass extension is compiled and linked in without the world stopping.

- [x] 3D on the GPU: Qualcomm's OpenGL ES 3.2 driver runs under afterglow (stereo wireframe floor, 4 ms/frame)
- [x] Centred on the lenses and corrected for their distortion and colour fringes (Meta's lens model); reprojected at every refresh
- [ ] Frames straight to the panels (today a readback and a 3-5 ms copy each refresh)
- [x] The world: render loop, scene (meshes, shaders, objects), API in [`sdk/afterglow.h`](system/world/sdk/afterglow.h)
- [x] Extensions as WebAssembly (WAMR), with `on_load()`/`on_frame()`/`on_unload()`, swapped while the
      world runs; a crashing or looping extension is stopped and undone, not the world
- [x] Compiled on the headset (clang in an Alpine root on `/storage`); afterglowd tools `load_extension`,
      `unload_extension`, `list_extensions`, `start_world`, `stop_world`; versions kept on `/storage`
- [x] Something on screen in seconds: placeholders of ready-made shapes, with a road-works sign until
      the real thing replaces them; changes sent as edits, not whole sources
- [x] [`shapes.h`](system/world/sdk/shapes.h) (vectors, transforms, meshes from boxes, spheres, cylinders,
      lathes; text labels), a lit standard shader, blending, textures (from pixels, files, text)
- [x] Textures from an image generator on the network (`generate_texture`, AUTOMATIC1111 web API) or
      the web (`fetch_texture`); made seamless when asked
- [x] Head pose in the world API (`head_pose()`), tracked by the IMU
- [x] Extensions see each other (`object_get_transform`) and keep state across versions and restarts
      (`remember`, `recall`), so a change reloads only what changed and carries on where it was
- [ ] Input in the world API: buttons now, controllers later
- [ ] Instancing (many copies of a mesh in one draw), and a raised GPU clock while the world runs
- [ ] Ahead-of-time compiled extensions (`wamrc`) for heavy per-frame code
- [ ] Voice: "make the floor grass" said in the headset
- [ ] Places: each one saved by name (its extensions and their versions, a picture), changes made
      while you're in it kept, and Claude tools to start, list and go back to them
- [ ] A console the wearer can call up from inside any place (as the holodeck has its arch): go to a
      saved place, go home, face forward, see what Claude is doing, switch off. afterglow's own look
      (brand/), chosen with the head buttons and gaze until controllers work

## Phase 3 — Claude-native

- [x] `afterglowd` 0.1: MCP server on the headset over Wi-Fi: shell, files, screenshot, logs, status;
      paired over USB; found as `afterglow.local` ([README](system/afterglowd/README.md))
- [ ] `afterglowd`: apps (create, run, stop) and sensors
- [ ] TLS for afterglowd (the token currently crosses the network in plain HTTP)
- [ ] Pairing code shown in the headset; approvals with the controller
- [ ] On-device API docs served as MCP resources

## Phase 4 — Installing

- [ ] Rebuild the inactive slot cleanly; afterglow in one slot, Android in the other
- [ ] Flash storage for apps
- [ ] Browser-based installer (fastboot over WebUSB)

## Phase 5+ — Toward feature parity

- [x] 3DoF head tracking from the motion sensor (the IMU behind SyncBoss; [docs/apps.md](docs/apps.md))
- [ ] An open GPU driver (Mesa freedreno, with mainline Linux); Qualcomm's runs today
- [ ] Bluetooth (Wi-Fi: done in Phase 1)
- [ ] Controllers (the Rift S shares them; see Monado's Rift S driver)
- [ ] 6DoF tracking with open SLAM (Basalt via Monado)
- [ ] Mainline Linux
