#!/bin/sh
# Build the world (out/world/world) with the Android NDK and WAMR, on the Mac:
#   system/world/build.sh
# WAMR (Apache-2.0 with LLVM exception) is downloaded once, checksum pinned.
set -eu
NDK=${NDK:-$PWD/tools/android-ndk-dl/android-ndk-r30}
OUT=$PWD/out/world
WAMR=$OUT/wasm-micro-runtime-WAMR-2.4.5
mkdir -p "$OUT"
if [ ! -d "$WAMR" ]; then
    curl -fsSL -o "$OUT/wamr.tar.gz" https://github.com/bytecodealliance/wasm-micro-runtime/archive/refs/tags/WAMR-2.4.5.tar.gz
    echo "1ab09d51099f276ca4a1d6629f6b589aab2bd0caa01445e05031a4bed22c199b  $OUT/wamr.tar.gz" | shasum -a 256 -c -
    tar -xzf "$OUT/wamr.tar.gz" -C "$OUT"
fi
cmake -S system/world -B "$OUT/build" -DCMAKE_BUILD_TYPE=Release -DWAMR_ROOT_DIR="$WAMR" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-29 > "$OUT/cmake.log" 2>&1
cmake --build "$OUT/build" -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" > "$OUT/build.log"
"$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip" -o "$OUT/world" "$OUT/build/world"
ls -l "$OUT/world"
