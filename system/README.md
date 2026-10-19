# afterglow system

afterglow's own userspace: a static `/init`, which runs as PID 1 on our kernel, starts the hardware
bring-up (the Snapdragon's subsystems, Wi-Fi, afterglowd) and plays the 3D boot animation until
that's done, then shows a status screen; BusyBox for a development shell over USB; and `afterglowd`,
the MCP server through which Claude Code on a computer works on the headset.

| File | What |
|---|---|
| `init/init.c` | PID 1: minimal `/proc`, `/sys`, `/dev`, loopback; starts the bring-up; reads options from the kernel command line; switches off when Power is pressed twice or on BusyBox's `poweroff` |
| `init/status.c` | The status screen: one line per file in `/run/afterglow/status` (`label<TAB>text`), in Montserrat via stb_truetype |
| `init/display.c` | Framebuffer access and per-eye drawing (the panels show the framebuffer rotated 180°; each eye's image can be turned to cancel panel roll) |
| `init/bootanim.c` | Plays a pre-rendered stereo animation (`tools/pack_anim.py` format), both eyes decoded and drawn in parallel |
| `init/calibrate.c` | Interactive display roll calibration with the headset's buttons |
| `init/input.c` | Buttons, read straight from the kernel's input devices |
| `init/usb.c` | USB serial console: the kernel log and commands on one port, a shell on the other |
| `tests/display_test.c` | Runs the real drawing code off-device against a fake framebuffer and writes what the viewer would see |
| `tools/logdw.c` | Forwards Android programs' log messages to the kernel log (a stand-in for Android's `logd`) |
| `scripts/bringup.sh` | Starts the audio DSP, GPU, modem DSP and Wi-Fi as Android would, with the headset's own firmware ([findings](../docs/hardware.md#subsystems-and-wi-fi-without-android)) |
| `scripts/wifi.sh`, `scripts/udhcpc.sh` | Scan, join and forget Wi-Fi networks, set the country; apply DHCP leases |
| `scripts/format_storage.sh` | Make afterglow's storage on the spare slot's system partition ([docs/storage.md](../docs/storage.md)) |
| `tools/worldctl.c` | `worldctl COMMAND`: a command to the running world's control socket (`list`, `recentre`, `test-tilt`, ...) |
| `tools/syncboss.c` | `syncboss [-t S] [-x] [-i N]`: switch on SyncBoss (the IMU's microcontroller), ask for IMU samples, report or decode the stream ([docs/hardware.md](../docs/hardware.md#the-imu-syncboss)) |
| `tools/restart.c` | `restart [bootloader]` from the shell or afterglowd (BusyBox's `reboot` and `poweroff` work too, but can't name a target) |
| `afterglowd/` | The MCP server: shell, files, screenshots, logs, status, the world's extensions, over Wi-Fi ([README](afterglowd/README.md)) |
| `world/` | The 3D world on the GPU, extended while it runs by WebAssembly extensions ([README](world/README.md)) |
| `gpu/` | The first GPU probe: Qualcomm's OpenGL ES driver under afterglow ([README](gpu/README.md)) |
| `scripts/wasm-cc.sh` | Compiles a world extension to WebAssembly with the compiler on `/storage/toolchain` |
| `third_party/` | BusyBox, wpa_supplicant and e2fsprogs (mke2fs, e2fsck) from upstream source, static on musl (native arm64 container), and the Go toolchain |
| `build.sh` | Builds `/init` (static aarch64, with zstd's decompressor), the tools, and the initramfs |

## Kernel command line options

| Option | Meaning |
|---|---|
| `afterglow_initramfs` | Kernel patch: use the boot image's ramdisk even though the bootloader passes `skip_initramfs` |
| `afterglow.roll_left_deg=N`, `afterglow.roll_right_deg=N` | Turn each eye's image N° counterclockwise to cancel panel roll (measured with the calibration screen) |
| `afterglow.bringup=0` | Don't start the bring-up at boot (the animation then keeps looping) |
| `afterglow.calibrate=1` | Show the calibration screen instead of the boot animation |
| `afterglow.test_seconds=N` | Development, for experiments that might hang: restart after N seconds (images otherwise run until told to stop) |
| `afterglow.restart_to=bootloader` | Development: restart into the fastboot menu |

## Build and try it

```sh
# Animation (see brand/boot3d); third-party programs (about 40 s, native arm64); /init + initramfs;
# then a boot image with our kernel
blender -b --factory-startup -P brand/boot3d/scene.py -- --out out/boot3d/stereo --res 900 --samples 48 --stereo
python3 tools/pack_anim.py out/boot3d/stereo out/boot3d/boot.anim --fps 36 --loop-start 109
docker build --platform linux/arm64 -t afterglow-userland system/third_party
docker run --rm -v "$PWD":/work afterglow-userland          # writes out/third_party
docker run --rm -v "$PWD":/work afterglow-userland /work/system/afterglowd/build.sh
system/world/build.sh                                       # Android NDK, on the Mac (world/README.md)
docker run --rm -v "$PWD":/work afterglow-kernel-build /work/system/build.sh /work/out/boot3d/boot.anim
docker run --rm -v "$PWD":/work afterglow-kernel-build /work/kernel/build/make_bootimg.sh \
    /work/firmware/49845030443200410/boot.img /work/out/kernel/Image.gz-dtb /work/out/system/boot.img \
    /work/out/system/initramfs.cpio.gz "afterglow_initramfs afterglow.roll_left_deg=1 afterglow.roll_right_deg=2"
fastboot boot out/system/boot.img     # RAM only; press Power twice to switch off (then it starts Android)
```

## USB serial console

As soon as `/init` starts, the headset appears on the computer it's plugged into as two USB serial
ports (CDC-ACM; on a Mac `/dev/cu.usbmodemafterglow1` and `…3`, on Linux `/dev/ttyACM*`):

1. the whole kernel log from the first line of the boot, and commands (`bootloader`, `reboot`, `poweroff`, `help`).
   Writes wait for the computer, so nothing is lost before it connects.
2. a BusyBox shell (1.37.0, static, about 400 tools), which `/init` restarts if it exits.

```sh
tools/console.py --log out/console.log            # waits for the port, prints the log live
tools/console.py --send bootloader                 # restart into fastboot
tools/console.py --run 'ls /sys/class/video4linux' # run one command; prints output, returns its status
tools/console.py --shell                           # connect to the shell port (for a full terminal: screen)
```

It needs `CONFIG_USB_CONFIGFS_ACM`, which stock Android doesn't use; `kernel/configs/afterglow.config`
adds it. Measured with it (2026-10-18): `/init` starts 2.4 s after the kernel, the port appears about
9 s after `fastboot boot`, and the boot animation decodes and draws a stereo frame in about 21 ms with
no late frames.

## Subsystems and Wi-Fi

`/afterglow/bringup.sh` starts the hardware that needs firmware, using the firmware and Qualcomm's
helper programs already on the headset (its `modem` and `system` partitions, mounted read-only;
anything they write goes to RAM). `/init` runs `bringup.sh all` at boot (log: `/tmp/bringup.log`;
`afterglow.bringup=0` on the kernel command line turns that off). A step that fails doesn't stop the
rest, so afterglowd still comes up to find out why.

Measured 2026-10-19, from the kernel starting: bring-up done at 20 s (Wi-Fi waits for the kernel's
random pool, seeded at about 20 s: there's no hardware RNG driver), Wi-Fi address at 27 s, afterglowd
answering at 30 s. The status screen then shows System, Wi-Fi and Claude; /init uses no CPU for it.

The first time, or for a new network:

```sh
tools/console.py --wifi                                  # asks for a network, password and country
tools/console.py --pair                                  # let this computer's Claude Code use afterglowd
tools/console.py --run 'sh /afterglow/wifi.sh status'
```

`--wifi` asks on the computer, so the password never appears on a command line, and sends it to the
headset as the WPA2 key derived from it, never the password itself. With afterglow storage
([docs/storage.md](../docs/storage.md)) the network is saved and the headset reconnects by itself
after a restart; without it, it's kept in RAM until then.
`wifi.sh scan` lists the networks in range. Single steps (`bringup.sh mounts android ...`) are there
for debugging; `sh /afterglow/bringup.sh` lists them.

`tools/console.py --push LOCAL:REMOTE` copies a small file into the headset's RAM over the shell,
which is handy for trying a changed script without rebuilding.

## Third-party programs

`third_party/Dockerfile` builds them from their upstream releases (checksums pinned; wpa_supplicant's
signature checked when it was added), statically linked on musl. musl matters here: a static glibc
program can't look up host names, because glibc loads its DNS code as shared libraries at run time.
Their licences go into the image under `/usr/share/licenses`. libnl is LGPL-2.1: if you distribute
images you build, also offer this build recipe so others can relink with a changed libnl.
