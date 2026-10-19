#!/bin/bash
# Build afterglow's initramfs: a static aarch64 /init, the boot animation, BusyBox and the other
# third-party programs. Runs inside the afterglow-kernel-build container (see kernel/build/README.md),
# after system/third_party has been built into out/third_party.
# Usage: system/build.sh <boot animation .anim>
set -euo pipefail

ANIM="${1:?boot animation .anim}"
OUT=/work/out/system
ROOT="$OUT/initramfs"
ZSTD=/opt/zstd/lib
STB=/opt/stb
THIRD=/work/out/third_party
AFTERGLOWD=/work/out/afterglowd/afterglowd
[ -x "$THIRD/bin/busybox" ] || { echo "no $THIRD: build system/third_party first (see system/README.md)" >&2; exit 1; }
[ -x "$AFTERGLOWD" ] || { echo "no $AFTERGLOWD: build it first (see system/README.md)" >&2; exit 1; }

rm -rf "$ROOT"
mkdir -p "$ROOT"/{dev,proc,sys,tmp,afterglow,bin,sbin,usr/bin,usr/sbin,usr/share}

# Third-party programs built from upstream source (system/third_party), with their licences. BusyBox
# is one binary with a link per applet, except where a full program replaces the applet (mke2fs).
install -m 0755 "$THIRD"/bin/* "$ROOT/bin/"
cp -r "$THIRD/share/licenses" "$ROOT/usr/share/"
while read -r applet; do
    [ "$applet" = /bin/busybox ] || [ -e "$THIRD/bin/${applet##*/}" ] || ln -sf /bin/busybox "$ROOT$applet"
done < "$THIRD/share/busybox/busybox.links"

# zstd's decompressor only; its assembly fast path is x86-only
OBJS=()
for src in /work/system/init/*.c; do
    obj="$OUT/$(basename "${src%.c}").o"
    aarch64-linux-gnu-gcc -c -O2 -Wall -Wextra -I"$ZSTD" -I"$STB" -o "$obj" "$src"
    OBJS+=("$obj")
done
aarch64-linux-gnu-gcc -static -O2 -pthread -I"$ZSTD" -DZSTD_DISABLE_ASM -w \
    -o "$OUT/init" "${OBJS[@]}" \
    $ZSTD/common/{debug,entropy_common,error_private,fse_decompress,xxhash,zstd_common}.c \
    $ZSTD/decompress/{huf_decompress,zstd_ddict,zstd_decompress,zstd_decompress_block}.c -lm
aarch64-linux-gnu-strip "$OUT/init"

# Small standalone helpers (system/tools/*.c) go in /bin
mkdir -p "$OUT/tools"
for src in /work/system/tools/*.c; do
    bin="$OUT/tools/$(basename "${src%.c}")"
    aarch64-linux-gnu-gcc -static -O2 -Wall -Wextra -o "$bin" "$src"
    aarch64-linux-gnu-strip "$bin"
done
cp "$OUT/init" "$ROOT/init"
cp "$ANIM" "$ROOT/afterglow/boot.anim"
# The status screen's typeface (SIL Open Font License)
cp /work/brand/fonts/Montserrat-Light-merged.ttf "$ROOT/afterglow/font.ttf"
mkdir -p "$ROOT/usr/share/licenses/montserrat"
cp /work/brand/fonts/OFL-Montserrat.txt "$ROOT/usr/share/licenses/montserrat/"
mkdir -p "$ROOT/usr/share/licenses/stb_truetype"
sed -n '/^This software is available under 2 licenses/,$p' "$STB/stb_truetype.h" > "$ROOT/usr/share/licenses/stb_truetype/LICENSE"
install -m 0755 /work/system/scripts/*.sh "$ROOT/afterglow/"
install -m 0755 "$OUT"/tools/* "$ROOT/bin/"
install -m 0755 "$AFTERGLOWD" "$ROOT/bin/"

# gzip-compressed: the headset's bootloader fails to boot an image whose ramdisk is much over 38 MiB
# (38.4 MiB booted; 39.8 MiB came straight back to fastboot, which then needed a power cycle). The
# animation is already compressed, but the programs shrink by more than half.
rm -f "$OUT/initramfs.cpio"
(cd "$ROOT" && find . | LC_ALL=C sort | cpio -o -H newc -R 0:0 --quiet) | gzip -9 -n > "$OUT/initramfs.cpio.gz"
ls -l "$OUT/init" "$OUT/initramfs.cpio.gz"
