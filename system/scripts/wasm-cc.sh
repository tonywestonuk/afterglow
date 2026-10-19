#!/bin/sh
# Compile a world extension (C) to WebAssembly with the compiler on /storage/toolchain (an Alpine root
# with clang and the WASI C library: system/world/toolchain.sh), run inside it with chroot. Compiler
# messages go to stderr and name the source file as given, or as NAME.c.
#   sh /afterglow/wasm-cc.sh SOURCE.c OUTPUT.wasm [NAME.c]
set -u
tc=/storage/toolchain
src=${1:?source .c}
out=${2:?output .wasm}
[ -x $tc/usr/bin/clang ] || { echo "no compiler on this headset: install it (system/world/README.md)" >&2; exit 2; }
[ -e $tc/dev/null ] || mknod -m 0666 $tc/dev/null c 1 3
grep -q " $tc/proc " /proc/mounts || mount -t proc proc $tc/proc

dir=$(mktemp -d $tc/build/XXXXXX) || exit 2
name=$(basename "${3:-$src}")
cp "$src" "$dir/$name" && cp /afterglow/sdk/afterglow.h "$dir/" || { rm -rf "$dir"; exit 2; }
chroot $tc /bin/sh -c "cd /build/${dir##*/} && /usr/bin/clang --target=wasm32-wasi -mexec-model=reactor -O2 \
    --sysroot=/usr/share/wasi-sysroot -I. -Wall -Wl,--export-if-defined=on_load \
    -Wl,--export-if-defined=on_frame -Wl,--export-if-defined=on_unload -o extension.wasm '$name'"
status=$?
[ $status -ne 0 ] || cp "$dir/extension.wasm" "$out" || status=2
rm -rf "$dir"
exit $status
