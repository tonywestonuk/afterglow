# Roadmap

afterglow is built in phases. Each phase ends with something that works and can be demonstrated.

## Architecture (target)

```
 Mac / PC                                   Quest 1 (afterglow)
┌────────────────────┐   USB or Wi-Fi   ┌──────────────────────────────┐
│ Claude Code         │ ◄──────────────► │ afterglowd (MCP server)       │
│ (or any MCP client) │   MCP over HTTP  │  ├─ apps: create/run/stop     │
└────────────────────┘                  │  ├─ files (app folders)       │
          │                              │  ├─ screenshot → image        │
          ▼                              │  ├─ logs, sensors, status     │
   Claude (cloud)                        │  └─ shell (opt-in, approved)  │
                                         ├──────────────────────────────┤
                                         │ App runtime: setup()/loop()   │
                                         │ C++ and JavaScript (QuickJS)  │
                                         ├──────────────────────────────┤
                                         │ Linux (start: Meta's 4.4      │
                                         │ kernel; later: mainline)      │
                                         └──────────────────────────────┘
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

## Phase 1 — Our own kernel on screen

- [ ] Boot a minimal Linux userspace (postmarketOS-style "downstream" port on Meta's kernel)
- [ ] Text console on the displays (framebuffer left by the bootloader)
- [ ] USB networking (NCM is already enabled in Meta's kernel config)
- [ ] SSH / shell over USB

## Phase 2 — The app platform

- [ ] Arduino-style app API: `setup()`, `loop()`, `millis()`, drawing, buttons
- [ ] Apps as files, loaded at runtime, isolated from the OS
- [ ] Stereo rendering helpers (one image per eye)
- [ ] JavaScript apps via QuickJS with instant reload

## Phase 3 — Claude-native

- [ ] `afterglowd`: MCP server on the headset (apps, files, screenshot, logs, sensors, gated shell)
- [ ] Pairing code shown in the headset; approvals with the controller
- [ ] On-device API docs served as MCP resources
- [ ] Prototype first on stock Android over adb, then move into afterglow

## Phase 4 — Installing

- [ ] Rebuild the inactive slot cleanly; afterglow in one slot, Android in the other
- [ ] Flash storage for apps
- [ ] Browser-based installer (fastboot over WebUSB)

## Phase 5+ — Toward feature parity

- [ ] 3DoF head tracking from the motion sensor
- [ ] GPU (Adreno 540; Mesa freedreno as reference)
- [ ] Wi-Fi and Bluetooth
- [ ] Controllers (the Rift S shares them; see Monado's Rift S driver)
- [ ] 6DoF tracking with open SLAM (Basalt via Monado)
- [ ] Mainline Linux
