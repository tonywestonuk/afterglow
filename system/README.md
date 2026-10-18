# afterglow system

afterglow's own userspace. For now that's a single static program, `/init`, which runs as PID 1 on our
kernel and plays the 3D boot animation, or the display calibration screen.

| File | What |
|---|---|
| `init/init.c` | PID 1: minimal `/proc`, `/sys`, `/dev`; reads options from the kernel command line; restarts in test builds |
| `init/display.c` | Framebuffer access and per-eye drawing (the panels show the framebuffer rotated 180°; each eye's image can be turned to cancel panel roll) |
| `init/bootanim.c` | Plays a pre-rendered stereo animation (`tools/pack_anim.py` format), both eyes decoded and drawn in parallel |
| `init/calibrate.c` | Interactive display roll calibration with the headset's buttons |
| `init/input.c` | Buttons, read straight from the kernel's input devices |
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

There is no log channel yet: pstore doesn't survive a restart into the bootloader, and USB isn't set
up. A USB serial console is the next step.
