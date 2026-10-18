# Building the Quest 1 kernel

Rebuilds Meta's published Quest 1 kernel (Linux 4.4.205) with the exact config of the stock kernel,
using the compiler Meta used. Tested 2026-10-18: the result boots stock Android on a retail Quest 1
via `fastboot boot`, and all 14 appended device trees are byte-identical to Meta's.

## Requirements

- Docker. On a Mac, [Colima](https://github.com/abiosoft/colima) with Rosetta works well:
  `colima start --vm-type vz --vz-rosetta --cpu 10 --memory 16`
  (Android's GCC 4.9 is x86_64-only, so the container is `linux/amd64` and runs under Rosetta.)
- `firmware/49845030443200410/boot.img`: the stock boot image extracted from Meta's update
  ([how](../../docs/rooting-and-backup.md#1-extract-the-stock-boot-image)).

## Build

```sh
docker build -t afterglow-kernel-build kernel/build
docker volume create afterglow-kernel-src
docker run --rm -v afterglow-kernel-src:/src -v "$PWD":/work afterglow-kernel-build /work/kernel/build/build.sh
```

The source is cloned into a Docker volume because Linux needs a case-sensitive filesystem. Rebuilds
are incremental. The first build took about 25 minutes in total on an M4 under Rosetta (across
restarts while fixing the issues below). Output: `out/kernel/Image.gz-dtb`.

Then wrap it with the stock ramdisk and header:

```sh
docker run --rm -v "$PWD":/work afterglow-kernel-build /work/kernel/build/make_bootimg.sh \
    /work/firmware/49845030443200410/boot.img /work/out/kernel/Image.gz-dtb /work/out/kernel/boot-test.img
fastboot boot out/kernel/boot-test.img      # RAM only; a normal restart returns to stock
adb shell cat /proc/version                 # shows your build host and date
```

## Gaps in Meta's published source, and how the build handles them

| Gap | Handling |
|---|---|
| `drivers/staging/oculus/internal/` is referenced by Kconfig and the Makefile but not published | Stubbed. No option enabled in the stock config depends on it (`out/kernel/config-missing.txt` is empty) |
| `CONFIG_SYSTEM_TRUSTED_KEYS="verity.x509.pem"` is not in the repo; without it `/system` can't be mounted | `extract_verity_cert.py` takes the certificate from the user's own stock kernel, matched by the `veritykeyid` on the command line. Never committed |
| The in-tree DTC 1.4.2 rejects `-Wno-simple_bus_reg` | Builds with the system DTC 1.4.5 via `DTC_EXT` |
| The compat vDSO needs a 32-bit compiler | Android's `arm-linux-androideabi-4.9` (`CROSS_COMPILE_ARM32`) |

The Android GCC wrappers are Python scripts that only print a deprecation notice; the container links
straight to the real compiler so parallel builds don't start Python for every file.
