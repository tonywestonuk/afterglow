# afterglow

**An open operating system for the Meta Quest 1.** Not affiliated with, or endorsed by, Meta.

Meta has ended support for the Quest 1, and most of them now sit in drawers. Since August 2026 its
bootloader can be unlocked, which makes it a surprisingly capable, hackable computer: a Snapdragon 835,
4 GB RAM, two 1440×1600 OLED displays, four tracking cameras, motion sensors and controllers.

afterglow aims to turn an unused Quest 1 into a device where **beginners can get something running in
minutes**, but where **anything can be built**:

- Arduino-style apps (`setup()` / `loop()`) in C++ or JavaScript, loaded from storage — no reflashing.
- Built for AI-assisted development from day one: the headset runs an [MCP](https://modelcontextprotocol.io)
  service, so Claude (running on your Mac or PC) can create, run, debug and screenshot apps on the headset.
- Linux underneath, so the long-term goal — feature parity with the original, including tracking — is
  realistic rather than a fantasy.

## Status

**Phase 0: research.** There is nothing to install yet. See [ROADMAP.md](ROADMAP.md).

Done so far:
- Confirmed the unlocked bootloader runs unsigned kernels from RAM with `fastboot boot` (nothing flashed).
- Temporary root on stock Android without flashing, and a full, checksum-verified partition backup.
- Identified the retail hardware (board `vs1 pvt b`, Samsung panels) and the matching files in
  [Meta's published kernel source](https://github.com/facebookincubator/oculus-linux-kernel/tree/oculus-quest-kernel-master).
- **Built that kernel from source and booted stock Android on it** ([kernel/build](kernel/build/README.md)).
- **Booted our own `/init` on it, with no Android: a pre-rendered 3D stereo boot animation**, corrected
  for the panels' 180° rotation and per-headset roll, plus button input ([system](system/README.md),
  [brand](brand/README.md)).
- A USB serial console with the live kernel log and a shell on the headset.
- **Wi-Fi, still with no Android running**: the Snapdragon's audio DSP, GPU and modem DSP started with
  their firmware, and the headset joins a WPA2 network and reaches the internet
  ([how](docs/hardware.md#subsystems-and-wi-fi-without-android)).
- **`afterglowd`, an MCP server on the headset**: Claude Code on a Mac runs commands, edits files, reads
  logs and sees the displays over Wi-Fi, paired over USB ([system/afterglowd](system/afterglowd/README.md)).
- **Its own storage**, on the spare slot's system partition: the headset remembers its Wi-Fi network and
  its pairing across restarts ([docs/storage.md](docs/storage.md)).
- **It comes up by itself**: about 30 s after `fastboot boot` it's on Wi-Fi with afterglowd answering, and
  the boot animation gives way to a status screen.
- **3D on the GPU, and a world Claude extends while it runs**: Qualcomm's OpenGL ES 3.2 driver on the
  Adreno 540; C extensions compiled to WebAssembly on the headset and swapped into the running world in
  under a second, with crashes contained and every change undoable ([docs/apps.md](docs/apps.md)).
- **Head tracking (turning)**, from the IMU behind Meta's SyncBoss microcontroller, read directly
  with no Android ([how](docs/hardware.md#the-imu-syncboss)).

Details: [docs/hardware.md](docs/hardware.md), [docs/rooting-and-backup.md](docs/rooting-and-backup.md).

## Requirements

- A Meta Quest 1 on firmware `49845030443200410` (the last release) with an **unlocked bootloader**,
  e.g. via [QuestStack](https://github.com/starseed12345/QuestStack) or the
  [web unlocker](https://quest1-unlock.skystate.ch/). Unlocking factory-resets the headset and
  permanently ends access to Meta's store and account services on it.
- A Mac or Linux PC with a **current** [Android platform-tools](https://developer.android.com/tools/releases/platform-tools)
  (old `fastboot` builds stall on large transfers).

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
| `tools/backup_partitions.sh` | Back up all partitions (except `userdata`) to your computer, sha256-verified |
| `tools/root-overlay/` | Magisk `overlay.d` files that pre-approve root for the adb shell (Quest can't show Magisk's prompt) |
| `kernel/configs/` | The exact config of Meta's running Quest 1 kernel (from `/proc/config.gz`) |
| `kernel/build/` | Reproducible kernel build (Docker, Meta's GCC 4.9) and boot-image packaging |
| `kernel/patches/` | afterglow's kernel changes (GPL-2.0) |
| `system/` | afterglow's `/init` (boot animation, display, buttons, calibration), subsystem and Wi-Fi bring-up, third-party userspace build |
| `brand/` | Logo, the 3D boot animation scene (Blender Python), fonts |
| `tools/pack_anim.py`, `tools/make_calibration_frames.py` | Pack rendered frames for the headset; calibration test patterns |
| `tools/console.py`, `tools/push_file.py` | USB console, shell, Wi-Fi setup and pairing; large files to the headset over Wi-Fi |
| `docs/` | Hardware notes and procedures |

## Licence

afterglow's own code is [MIT](LICENSE). Changes to the Linux kernel are GPL-2.0, like the kernel itself.
Boot images also include third-party programs built from their upstream releases by
`system/third_party/Dockerfile`: BusyBox (GPL-2.0), wpa_supplicant (BSD-3-Clause) and e2fsprogs
(GPL-2.0), statically linked with musl (MIT), libnl (LGPL-2.1) and OpenSSL (Apache-2.0); `/init` uses
stb_truetype (public domain / MIT) and the Montserrat typeface (SIL OFL). Their licences are in the
image under `/usr/share/licenses`.
Meta's firmware is not ours to distribute; tools extract it from each user's own headset or from Meta's
official update files.

## Credits

- [QuestStack](https://github.com/starseed12345/QuestStack) and darknight1050's
  [quest-bootloader-unlocker](https://github.com/darknight1050/quest-bootloader-unlocker), which made this possible.
- [Magisk](https://github.com/topjohnwu/Magisk), [postmarketOS](https://postmarketos.org) and
  [Monado](https://monado.freedesktop.org), whose work we build on and learn from.
