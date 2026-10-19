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

On the reference headset, `system_b` is afterglow's storage (formatted 2026-10-19 with the user's
approval, [docs/storage.md](docs/storage.md)), mounted at `/storage`: files there may be written freely.
Formatting or restoring the partition itself still needs asking.

Never commit Meta's firmware, partition images, backups or third-party binaries (see `.gitignore`), and
never put a device serial number in the repo.

## Tools and quirks

- Use `tools/platform-tools-dl/platform-tools/{adb,fastboot}` (current release). Old fastboot builds
  stall at `Sending` and leave the bootloader stuck until the headset is restarted.
- The bootloader fails to boot an image whose ramdisk is much over 38 MiB (keep it gzipped, as
  `system/build.sh` does), and after a failed boot or an aborted transfer it stops answering on USB.
  Only a power cycle fixes that: ask the user to hold power ~15 s, then Volume Down + Power.
- To restart from afterglow: `restart bootloader` (shell or afterglowd's `run`), or `--send bootloader`
  on the console's first port.
- When the headset shows its fastboot menu, tell the user **not to select anything**; one option is
  Factory Reset.
- The headset suspends when not worn. Long transfers need a wake lock
  (`echo name > /sys/power/wake_lock` as root) and `adb pull`, not long `adb exec-out` streams.
- When afterglow's own `/init` is running, use the USB serial console for logs and control:
  `tools/console.py --log out/console.log` (live kernel log), `--send bootloader` to end a test,
  `--run 'COMMAND'` to run a command in the headset's BusyBox shell. Console logs contain the
  device serial number: keep them in `out/`. `/init` runs `bringup.sh all` itself at boot (log
  `/tmp/bringup.log`); don't run it again by hand.
  Test boot images carry `afterglow.test_seconds=N afterglow.restart_to=bootloader` as a safety net.
- Once `afterglowd` is up and paired (`tools/console.py --pair`, see `system/afterglowd/README.md`), it's
  the faster way in: MCP tools over Wi-Fi at `http://afterglow.local:7070/mcp`, token in
  `~/.config/afterglow/token` (never print it). Its `write_file` refuses anything under `/dev`; test
  guards like that only with harmless paths (`/dev/null`, made-up names in a temp folder), never with
  a real disk such as `/dev/sda`.
- The 3D world: `load_extension` with C source against `system/world/sdk/afterglow.h` and `shapes.h` (on
  the headset in `/afterglow/sdk/`), then `screenshot`. It's compiled on the headset by the toolchain on
  `/storage/toolchain` (install: `system/world/README.md`). The NDK lives in `tools/android-ndk-dl/`.
  Placeholder first (`placeholder: true`) when the real thing takes long to write; `edits` for changes.
  To try a new world binary without rebooting: `stop_world`, replace `/afterglow/world` (RAM), start again.
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
- Build and package with `kernel/build/` ([README](kernel/build/README.md)); it handles the gaps in Meta's
  published tree (unpublished `internal/` driver dir, verity certificate, DTC version, 32-bit compiler).
  Verified: rebuilt kernel boots stock Android via `fastboot boot`, all 14 DTBs byte-identical to stock.

## Conventions

- Record new hardware findings in `docs/hardware.md`, and procedures in `docs/`, as soon as they're confirmed.
  Mark anything unverified as such.
- Keep the ROADMAP checkboxes current.
- Our code is MIT. Kernel changes are GPL-2.0 and live under `kernel/`.
