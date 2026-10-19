#!/bin/sh
# Build the GPU probe with the Android NDK: Android's C library, API level 29 (the headset's
# Android 10). Runs on the Mac: system/gpu/build.sh   Output: out/gpu/gles_probe
set -eu
NDK=${NDK:-tools/android-ndk-dl/android-ndk-r30}
CC=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android29-clang
mkdir -p out/gpu
"$CC" -O2 -Wall -Wextra -o out/gpu/gles_probe system/gpu/gles_probe.c -ldl -lm
ls -l out/gpu/gles_probe
