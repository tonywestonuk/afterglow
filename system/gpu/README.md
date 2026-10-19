# GPU

Programs that draw with the Adreno 540 through Qualcomm's own OpenGL ES driver, from the headset's
Android partition ([docs/apps.md](../../docs/apps.md#the-gpu-proven-2026-10-19)).

| File | What |
|---|---|
| `gles_probe.c` | Loads the driver, draws a wireframe floor in stereo offscreen, reports the speed, copies the frame to the screen |
| `build.sh` | Builds with the Android NDK (r30, API 29) on the Mac |

The NDK goes in `tools/android-ndk-dl/` (not in git): `android-ndk-r30-darwin.zip` from
`https://dl.google.com/android/repository/`, SHA-1 `c060be96767eefbb8e0a27796d6f43115fc1a0c4`.

```sh
system/gpu/build.sh                                  # out/gpu/gles_probe
# copy it to the headset (afterglowd write_file, or tools/console.py --push), then there:
LD_LIBRARY_PATH=/system/lib64/vndk-sp-29:/system/lib64/vndk-29:/vendor/lib64:/vendor/lib64/egl /tmp/gles_probe
```

It needs `bringup.sh gpu` (part of the boot): the GPU powered, `/dev/ion`, and Qualcomm's gralloc
where Android's HAL loader looks without system properties.
