#!/bin/bash
# Wrap a kernel (Image.gz-dtb) with the stock ramdisk into a boot image, copying every header field
# (load addresses, page size, cmdline, OS version/patch level) from the stock boot.img.
# Runs inside the afterglow-kernel-build container.
# Usage: make_bootimg.sh <stock boot.img> <Image.gz-dtb> <output boot image>
set -euo pipefail

STOCK="${1:?stock boot.img}"; KERNEL="${2:?Image.gz-dtb}"; OUTPUT="${3:?output image}"
WORK=$(mktemp -d)

# Boot image header v0: magic, then kernel/ramdisk/second sizes and load addresses, tags address,
# page size, header version, os_version, name[16], cmdline[512], id[32], extra_cmdline[1024]
eval "$(python3 - "$STOCK" "$WORK/ramdisk" <<'EOF'
import struct, sys
d = open(sys.argv[1], "rb").read()
assert d[:8] == b"ANDROID!", "not an Android boot image"
ks, ka, rs, ra, ss, sa, ta, ps, hv, osv = struct.unpack_from("<10I", d, 8)
assert hv == 0, f"expected header v0, got v{hv}"
pages = lambda n: (n + ps - 1) // ps
r0 = ps * (1 + pages(ks))
open(sys.argv[2], "wb").write(d[r0:r0 + rs])
v, lvl = osv >> 11, osv & 0x7ff
cmdline = (d[64:576].rstrip(b"\0") + d[608:1632].rstrip(b"\0")).decode()
print(f"PAGESIZE={ps} KERNEL_ADDR={ka:#x} RAMDISK_ADDR={ra:#x} SECOND_ADDR={sa:#x} TAGS_ADDR={ta:#x}")
print(f"OS_VERSION={(v>>14)&0x7f}.{(v>>7)&0x7f}.{v&0x7f} OS_PATCH={(lvl>>4)+2000}-{lvl&0xf:02d}")
print("CMDLINE='" + cmdline.replace("'", "'\\''") + "'")
EOF
)"

# With --base 0 each offset is the absolute load address
mkbootimg --header_version 0 --pagesize "$PAGESIZE" --base 0 \
    --kernel_offset "$KERNEL_ADDR" --ramdisk_offset "$RAMDISK_ADDR" \
    --second_offset "$SECOND_ADDR" --tags_offset "$TAGS_ADDR" \
    --os_version "$OS_VERSION" --os_patch_level "$OS_PATCH" \
    --cmdline "$CMDLINE" --kernel "$KERNEL" --ramdisk "$WORK/ramdisk" -o "$OUTPUT"

rm -rf "$WORK"
echo "wrote $OUTPUT"
