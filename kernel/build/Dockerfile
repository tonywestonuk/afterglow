# Build environment for Meta's Quest 1 kernel (Linux 4.4.205).
# x86_64 because Android's prebuilt GCC 4.9 is x86_64-only; on Apple Silicon this runs under Rosetta.
FROM --platform=linux/amd64 ubuntu:18.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        bc bison flex build-essential libssl-dev libelf-dev cpio kmod xz-utils device-tree-compiler \
        python python3 git ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

# The compiler Meta used ("gcc version 4.9.x 20150123 (prerelease)" in /proc/version), plus the
# 32-bit one the stock config needs for the compat vDSO (CROSS_COMPILE_ARM32)
RUN for tc in aarch64/aarch64-linux-android-4.9 arm/arm-linux-androideabi-4.9; do \
        mkdir -p /opt/${tc#*/} \
        && curl -fsSL https://android.googlesource.com/platform/prebuilts/gcc/linux-x86/$tc/+archive/refs/heads/android10-release.tar.gz \
           | tar -xz -C /opt/${tc#*/} || exit 1; \
    done
# Each <triple>-gcc is a Python wrapper that only prints a deprecation notice and execs the real
# driver (named by a UUID inside the wrapper); call the driver directly so parallel builds don't
# start Python (under Rosetta) for every file
RUN for gcc in /opt/aarch64-linux-android-4.9/bin/aarch64-linux-android-gcc \
               /opt/arm-linux-androideabi-4.9/bin/arm-linux-androideabi-gcc; do \
        real=$(grep -o -E '"[0-9a-f]{8}-[0-9a-f-]{27}"' "$gcc" | tr -d '"') \
        && [ -x "$(dirname "$gcc")/$real" ] \
        && mv "$gcc" "$gcc.deprecation-wrapper" && ln -s "$real" "$gcc" || exit 1; \
    done

# mkbootimg, to wrap the kernel with the stock ramdisk into a bootable image
RUN curl -fsSL -o /tmp/mkbootimg.b64 "https://android.googlesource.com/platform/system/tools/mkbootimg/+/refs/heads/android11-release/mkbootimg.py?format=TEXT" \
    && base64 -d /tmp/mkbootimg.b64 > /usr/local/bin/mkbootimg && rm /tmp/mkbootimg.b64 \
    && sed -i '1s|.*|#!/usr/bin/env python3|' /usr/local/bin/mkbootimg && chmod +x /usr/local/bin/mkbootimg \
    && mkbootimg --help > /dev/null

# Userspace: static aarch64 binaries for afterglow's init (system/), with zstd for frame decompression
RUN apt-get update && apt-get install -y --no-install-recommends gcc-aarch64-linux-gnu libc6-dev-arm64-cross \
    && rm -rf /var/lib/apt/lists/*
RUN curl -fsSL -o /tmp/zstd.tar.gz https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz \
    && echo "eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3  /tmp/zstd.tar.gz" | sha256sum -c - \
    && tar -xzf /tmp/zstd.tar.gz -C /opt && mv /opt/zstd-1.5.7 /opt/zstd && rm /tmp/zstd.tar.gz

ENV PATH=/opt/aarch64-linux-android-4.9/bin:/opt/arm-linux-androideabi-4.9/bin:$PATH \
    ARCH=arm64 \
    CROSS_COMPILE=aarch64-linux-android- \
    CROSS_COMPILE_ARM32=arm-linux-androideabi-

WORKDIR /src
