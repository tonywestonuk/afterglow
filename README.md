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
| `docs/` | Hardware notes and procedures |

## Licence

afterglow's own code is [MIT](LICENSE). Changes to the Linux kernel are GPL-2.0, like the kernel itself.
Meta's firmware is not ours to distribute; tools extract it from each user's own headset or from Meta's
official update files.

## Credits

- [QuestStack](https://github.com/starseed12345/QuestStack) and darknight1050's
  [quest-bootloader-unlocker](https://github.com/darknight1050/quest-bootloader-unlocker), which made this possible.
- [Magisk](https://github.com/topjohnwu/Magisk), [postmarketOS](https://postmarketos.org) and
  [Monado](https://monado.freedesktop.org), whose work we build on and learn from.
