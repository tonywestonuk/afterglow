# CLAUDE.md

afterglow is an open OS for the Meta Quest 1: Linux underneath, an Arduino-style app platform on top,
and an MCP service (`afterglowd`) so Claude on a Mac/PC can build and run apps on the headset.
See [README.md](README.md), [ROADMAP.md](ROADMAP.md) and [docs/hardware.md](docs/hardware.md).

## Hard rules for working with a headset

These are a real person's device with irreplaceable per-device data. Ask before any of these, every time:

- Writing any partition (`fastboot flash`, `dd of=/dev/block/...`, `fastboot erase`).
- `fastboot set_active` — slot B on unlocked headsets is a mismatched 2020 boot chain.
- `fastboot oem lock` / `flashing lock`, factory reset, or anything that relocks or wipes.
- Restarting the headset, or anything else that changes its state.

Never write `persist`, `private`, `vision`, `devinfo`, `modemst1/2`, `fsg`, `fsc`. Test kernels and images
with `fastboot boot` (RAM only). Back up before anything new (`tools/backup_partitions.sh`).

Never commit Meta's firmware, partition images, backups or third-party binaries (see `.gitignore`), and
never put a device serial number in the repo.

## Tools and quirks

- Use `tools/platform-tools-dl/platform-tools/{adb,fastboot}` (current release). Old fastboot builds
  stall at `Sending` and leave the bootloader stuck until the headset is restarted.
- When the headset shows its fastboot menu, tell the user **not to select anything**; one option is
  Factory Reset.
- The headset suspends when not worn. Long transfers need a wake lock
  (`echo name > /sys/power/wake_lock` as root) and `adb pull`, not long `adb exec-out` streams.
- Magisk's root prompt can't display on a Quest with a custom launcher. Use the root overlay in
  `tools/root-overlay/` ([procedure](docs/rooting-and-backup.md)).
- macOS has no `timeout`, and Bash is 3.2: keep scripts portable.
- macOS's case-insensitive filesystem corrupts Linux kernel trees. Clone and build kernels inside a Linux
  environment (container or VM), not directly on APFS.

## Kernel

- Source: `facebookincubator/oculus-linux-kernel`, branch `oculus-quest-kernel-master` (Linux 4.4.205).
- Board: `arch/arm64/boot/dts/oculus/vs1-pvt-b-sdc.dts`; panel: `dsi-panel-sdc-lightman-video.dtsi`.
- Exact running config: `kernel/configs/vs1-pvt-b-sdc_4.4.205-perf_running.config`. Meta built with GCC 4.9.
- Boot image: header v0, page size 4096, gzip kernel + 14 appended DTBs, legacy system-as-root.

## Conventions

- Record new hardware findings in `docs/hardware.md`, and procedures in `docs/`, as soon as they're confirmed.
  Mark anything unverified as such.
- Keep the ROADMAP checkboxes current.
- Our code is MIT. Kernel changes are GPL-2.0 and live under `kernel/`.
