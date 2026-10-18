# afterglow system

afterglow's own userspace. For now that's a static program, `/init`, which runs as PID 1 on our kernel
and plays the 3D boot animation (or the display calibration screen), plus BusyBox for a development
shell over USB.

| File | What |
|---|---|
| `init/init.c` | PID 1: minimal `/proc`, `/sys`, `/dev`; reads options from the kernel command line; restarts in test builds |
| `init/display.c` | Framebuffer access and per-eye drawing (the panels show the framebuffer rotated 180°; each eye's image can be turned to cancel panel roll) |
| `init/bootanim.c` | Plays a pre-rendered stereo animation (`tools/pack_anim.py` format), both eyes decoded and drawn in parallel |
| `init/calibrate.c` | Interactive display roll calibration with the headset's buttons |
| `init/input.c` | Buttons, read straight from the kernel's input devices |
| `init/usb.c` | USB serial console: the kernel log and commands on one port, a shell on the other |
| `tests/display_test.c` | Runs the real drawing code off-device against a fake framebuffer and writes what the viewer would see |
| `build.sh` | Builds `/init` (static aarch64, with zstd's decompressor) and the initramfs |

## Kernel command line options

| Option | Meaning |
|---|---|
| `afterglow_initramfs` | Kernel patch: use the boot image's ramdisk even though the bootloader passes `skip_initramfs` |
| `afterglow.roll_left_deg=N`, `afterglow.roll_right_deg=N` | Turn each eye's image N° counterclockwise to cancel panel roll (measured with the calibration screen) |
| `afterglow.calibrate=1` | Show the calibration screen instead of the boot animation |
| `afterglow.test_seconds=N` | Development: restart after N seconds |
| `afterglow.restart_to=bootloader` | Development: restart into the fastboot menu |

## Build and try it

```sh
# Animation (see brand/boot3d), then /init + initramfs, then a boot image with our kernel
blender -b --factory-startup -P brand/boot3d/scene.py -- --out out/boot3d/stereo --res 900 --samples 48 --stereo
python3 tools/pack_anim.py out/boot3d/stereo out/boot3d/boot.anim --fps 36 --loop-start 109
docker run --rm -v "$PWD":/work afterglow-kernel-build /work/system/build.sh /work/out/boot3d/boot.anim
docker run --rm -v "$PWD":/work afterglow-kernel-build /work/kernel/build/make_bootimg.sh \
    /work/firmware/49845030443200410/boot.img /work/out/kernel/Image.gz-dtb /work/out/system/boot.img \
    /work/out/system/initramfs.cpio "afterglow_initramfs afterglow.roll_left_deg=1 afterglow.roll_right_deg=2"
fastboot boot out/system/boot.img     # RAM only; hold power ~10 s to get back to Android
```

## USB serial console

As soon as `/init` starts, the headset appears on the computer it's plugged into as two USB serial
ports (CDC-ACM; on a Mac `/dev/cu.usbmodemafterglow1` and `…3`, on Linux `/dev/ttyACM*`):

1. the whole kernel log from the first line of the boot, and commands (`bootloader`, `reboot`, `help`).
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
