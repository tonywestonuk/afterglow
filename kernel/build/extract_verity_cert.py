#!/usr/bin/env python3
"""Extract the dm-verity certificate built into a stock Quest 1 kernel.

The stock config sets CONFIG_SYSTEM_TRUSTED_KEYS="verity.x509.pem", and the bootloader passes
veritykeyid=id:<subject key id>, so a rebuilt kernel needs the same public certificate to mount
/system. It is Meta's, so it is extracted from the user's own boot.img rather than committed.

Usage: extract_verity_cert.py <stock boot.img> <output verity.x509.pem>
"""
import base64
import re
import struct
import sys
import zlib


def der_length(buf, i):
    """Return (content length, header length) of the DER element starting at buf[i]."""
    first = buf[i + 1]
    if first < 0x80:
        return first, 2
    n = first & 0x7F
    return int.from_bytes(buf[i + 2:i + 2 + n], "big"), 2 + n


def main(boot_img, out_pem):
    data = open(boot_img, "rb").read()
    kernel_size, page_size = struct.unpack_from("<I", data, 8)[0], struct.unpack_from("<I", data, 36)[0]
    kernel_gz = data[page_size:page_size + kernel_size]
    # Image.gz-dtb: a gzip stream followed by appended device trees, which decompressobj leaves unused
    kernel = zlib.decompressobj(wbits=31).decompress(kernel_gz)

    cmdline = (data[64:576].rstrip(b"\0") + data[608:1632].rstrip(b"\0")).decode()
    wanted = re.search(r"veritykeyid=id:([0-9a-f]+)", cmdline)
    wanted = bytes.fromhex(wanted.group(1)) if wanted else None

    # An X.509 certificate is SEQUENCE { SEQUENCE tbsCertificate, ... }: 30 82 xx xx 30 82
    for m in re.finditer(rb"\x30\x82..\x30\x82", kernel, re.DOTALL):
        length, header = der_length(kernel, m.start())
        cert = kernel[m.start():m.start() + header + length]
        # Subject Key Identifier extension (OID 2.5.29.14) holds an OCTET STRING with the key id
        skid = re.search(rb"\x06\x03\x55\x1d\x0e\x04\x16\x04\x14(.{20})", cert, re.DOTALL)
        if wanted and not (skid and skid.group(1) == wanted):
            continue
        b64 = base64.encodebytes(cert).decode().replace("\n", "")
        pem = "-----BEGIN CERTIFICATE-----\n"
        pem += "\n".join(b64[i:i + 64] for i in range(0, len(b64), 64))
        pem += "\n-----END CERTIFICATE-----\n"
        open(out_pem, "w").write(pem)
        print(f"wrote {out_pem}: {len(cert)} bytes, subject key id {skid.group(1).hex() if skid else '?'}")
        return 0

    print("no certificate matching veritykeyid found in the kernel", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:3]))
