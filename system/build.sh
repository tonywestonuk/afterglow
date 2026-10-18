#!/bin/bash
# Build afterglow's initramfs: a static aarch64 /init, the boot animation, and BusyBox.
# Runs inside the afterglow-kernel-build container (see kernel/build/README.md).
# Usage: system/build.sh <boot animation .anim>
set -euo pipefail

ANIM="${1:?boot animation .anim}"
OUT=/work/out/system
ROOT="$OUT/initramfs"
ZSTD=/opt/zstd/lib

rm -rf "$ROOT"
mkdir -p "$ROOT"/{dev,proc,sys,tmp,afterglow,bin,sbin,usr/bin,usr/sbin}

# BusyBox (built in the container image) for the development shell: one binary, a link per applet
cp /opt/busybox/busybox "$ROOT/bin/busybox"
while read -r applet; do
    [ "$applet" = /bin/busybox ] || ln -sf /bin/busybox "$ROOT$applet"
done < /opt/busybox/busybox.links

# zstd's decompressor only; its assembly fast path is x86-only
OBJS=()
for src in /work/system/init/*.c; do
    obj="$OUT/$(basename "${src%.c}").o"
    aarch64-linux-gnu-gcc -c -O2 -Wall -Wextra -I"$ZSTD" -o "$obj" "$src"
    OBJS+=("$obj")
done
aarch64-linux-gnu-gcc -static -O2 -pthread -I"$ZSTD" -DZSTD_DISABLE_ASM -w \
    -o "$OUT/init" "${OBJS[@]}" \
    $ZSTD/common/{debug,entropy_common,error_private,fse_decompress,xxhash,zstd_common}.c \
    $ZSTD/decompress/{huf_decompress,zstd_ddict,zstd_decompress,zstd_decompress_block}.c -lm
aarch64-linux-gnu-strip "$OUT/init"
cp "$OUT/init" "$ROOT/init"
cp "$ANIM" "$ROOT/afterglow/boot.anim"

# Uncompressed cpio: the animation is already zstd-compressed
(cd "$ROOT" && find . | LC_ALL=C sort | cpio -o -H newc -R 0:0 --quiet) > "$OUT/initramfs.cpio"
ls -l "$OUT/init" "$OUT/initramfs.cpio"
