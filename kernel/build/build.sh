#!/bin/bash
# Build Meta's Quest 1 kernel with the exact config of the running stock kernel.
# Runs inside the afterglow-kernel-build container (see kernel/build/README.md): the source lives in a
# Docker volume at /src (Linux needs a case-sensitive filesystem), results go to the repo's out/kernel.
set -euo pipefail

SRC=/src/oculus-linux-kernel
BRANCH=oculus-quest-kernel-master
CONFIG=/work/kernel/configs/vs1-pvt-b-sdc_4.4.205-perf_running.config
FIRMWARE=/work/firmware/49845030443200410
OUT=/work/out/kernel
JOBS=$(( $(nproc) + 2 ))

if [ ! -d "$SRC/.git" ]; then
    git clone --depth 1 --branch "$BRANCH" https://github.com/facebookincubator/oculus-linux-kernel.git "$SRC"
fi
echo "source: $(git -C "$SRC" log -1 --format='%h %cd %s' --date=short)"

# Meta's published tree sources drivers/staging/oculus/internal/{Kconfig,Makefile}, which were never
# published. Stub them so the build can proceed; options they defined are reported below.
INTERNAL="$SRC/drivers/staging/oculus/internal"
if [ ! -e "$INTERNAL" ] || grep -q "Stub: not published" "$INTERNAL/Kconfig"; then
    mkdir -p "$INTERNAL"
    echo "# Stub: not published by Meta" > "$INTERNAL/Kconfig"
    # Kbuild only emits built-in.o for a directory with some object listed, even one not built (obj-)
    printf '# Stub: not published by Meta\nobj- := stub.o\n' > "$INTERNAL/Makefile"
fi

mkdir -p "$SRC/out" "$OUT"
cp "$CONFIG" "$SRC/out/.config"
make -C "$SRC" O=out olddefconfig

# Options enabled in the running stock kernel that this source tree can't provide
comm -23 <(grep -E '^CONFIG_[A-Z0-9_]+=[ym]' "$CONFIG" | sort) \
         <(grep -E '^CONFIG_[A-Z0-9_]+=[ym]' "$SRC/out/.config" | sort) > "$OUT/config-missing.txt" || true
echo "options in the stock config this tree can't build: $(wc -l < "$OUT/config-missing.txt")"
cat "$OUT/config-missing.txt"

# CONFIG_SYSTEM_TRUSTED_KEYS="verity.x509.pem": Android's /system can't be mounted without Meta's
# dm-verity certificate, so take it from the user's own stock kernel (never committed)
if [ ! -f "$FIRMWARE/verity.x509.pem" ]; then
    python3 /work/kernel/build/extract_verity_cert.py "$FIRMWARE/boot.img" "$FIRMWARE/verity.x509.pem"
fi
cp "$FIRMWARE/verity.x509.pem" "$SRC/out/verity.x509.pem"

# The in-tree DTC 1.4.2 predates the -Wno-simple_bus_reg flag Meta's dts build passes; use the system one
make -C "$SRC" O=out -j"$JOBS" DTC_EXT=dtc Image.gz-dtb

cp "$SRC/out/arch/arm64/boot/Image.gz-dtb" "$OUT/"
# grep stopping at the first match makes `strings` exit on SIGPIPE, which pipefail would report
version=$(strings "$SRC/out/vmlinux" | grep '^Linux version' | head -1) || true
echo "$version" | tee "$OUT/version.txt"
