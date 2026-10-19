#!/bin/sh
# The compiler that turns extensions (C) into WebAssembly on the headset: clang, lld and the WASI C
# library, installed by Alpine's package manager into a root of their own, packed as
# out/world/toolchain.tar.gz. It goes on /storage/toolchain, where wasm-cc.sh runs it with chroot.
# Runs in an Alpine arm64 container (packages are signature-checked by apk):
#   docker run --rm --platform linux/arm64 -v "$PWD":/work alpine:3.22 /work/system/world/toolchain.sh
set -eu
root=/tmp/toolchain
apk add --root "$root" --initdb --no-cache --no-scripts --keys-dir /etc/apk/keys \
    --repositories-file /etc/apk/repositories \
    busybox musl clang20 lld20 wasi-libc wasi-compiler-rt > /dev/null
ln -sf clang-20 "$root/usr/bin/clang" 2>/dev/null || true
ln -sf busybox "$root/bin/sh"   # normally made by BusyBox's install script, which --no-scripts skips
# Alpine's clang depends on gcc and binutils for native code; WebAssembly needs neither, nor native
# static libraries or link-time-optimisation plugins
rm -rf "$root/var/cache/apk"/* "$root/usr/share/doc" "$root/usr/share/man" "$root/usr/libexec/gcc" \
    "$root/usr/lib/gcc" "$root"/usr/lib/*.a "$root/usr/include/c++" "$root/usr/lib/llvm20/lib/LLVMgold.so" \
    "$root"/usr/lib/llvm20/lib/libLTO.so*
mkdir -p "$root/dev" "$root/proc" "$root/tmp" "$root/build"
du -sh "$root"
tar -czf /work/out/world/toolchain.tar.gz -C "$root" .
ls -l /work/out/world/toolchain.tar.gz
