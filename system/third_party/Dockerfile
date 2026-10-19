# Third-party userspace for afterglow, built from upstream source as static aarch64 binaries, and
# the Go toolchain that builds afterglowd (system/afterglowd/build.sh).
# Native arm64 (no Rosetta), on Alpine: musl, and static builds of the libraries they link.
#   docker build --platform linux/arm64 -t afterglow-userland system/third_party
#   docker run --rm -v "$PWD":/work afterglow-userland      # copies the results to out/third_party/
FROM --platform=linux/arm64 alpine:3.22

RUN apk add --no-cache build-base linux-headers pkgconf curl xz go \
        libnl3-dev libnl3-static openssl-dev openssl-libs-static

# e2fsprogs 1.47.4 (GPL-2.0; libraries LGPL/BSD/MIT): mke2fs and e2fsck for afterglow's storage.
# Upstream release; signature checked 2026-10-19 against Theodore Ts'o's key
# 3AB057B7E78D945C8C5591FBD36F769BC11804F0 (signing subkey 2B69B954DBFE0879288137C9F2F95956950D81A3).
RUN curl -fsSL -o /tmp/e2fs.tar.xz https://mirrors.edge.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v1.47.4/e2fsprogs-1.47.4.tar.xz \
    && echo "fd5bf388cbdbe006a3d3b318d983b2948382440acc85a87f1e7d108653e8db0b  /tmp/e2fs.tar.xz" | sha256sum -c - \
    && tar -xJf /tmp/e2fs.tar.xz -C /tmp && cd /tmp/e2fsprogs-1.47.4 \
    && ./configure --disable-nls --disable-fuse2fs --disable-uuidd --disable-fsck \
           --enable-libuuid --enable-libblkid LDFLAGS=-static > /dev/null \
    && make -j"$(nproc)" > /dev/null \
    && mkdir -p /opt/out/bin /opt/out/share/licenses/e2fsprogs \
    && install -s -m 0755 misc/mke2fs e2fsck/e2fsck /opt/out/bin/ \
    && cp NOTICE /opt/out/share/licenses/e2fsprogs/ \
    && cd / && rm -rf /tmp/e2fs*

# BusyBox 1.37.0 (GPL-2.0) for the shell and its tools: the upstream release as mirrored by Debian
# (busybox.net is often unreachable). Built on musl, whose resolver reads /etc/resolv.conf itself; a
# static glibc build can't look up names (glibc loads its DNS code as shared libraries). Left out: tc,
# which fails to build against current kernel headers, and the SHA hardware acceleration (x86-only).
RUN curl -fsSL -o /tmp/busybox.tar.bz2 https://deb.debian.org/debian/pool/main/b/busybox/busybox_1.37.0.orig.tar.bz2 \
    && echo "3311dff32e746499f4df0d5df04d7eb396382d7e108bb9250e7b519b837043a4  /tmp/busybox.tar.bz2" | sha256sum -c - \
    && tar -xjf /tmp/busybox.tar.bz2 -C /tmp && cd /tmp/busybox-1.37.0 \
    && make defconfig > /dev/null \
    && sed -i -e 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' -e 's/^CONFIG_TC=y/# CONFIG_TC is not set/' \
              -e 's/^CONFIG_SHA1_HWACCEL=y/# CONFIG_SHA1_HWACCEL is not set/' \
              -e 's/^CONFIG_SHA256_HWACCEL=y/# CONFIG_SHA256_HWACCEL is not set/' .config \
    && make -j"$(nproc)" busybox busybox.links > /dev/null \
    && mkdir -p /opt/out/bin /opt/out/share/busybox /opt/out/share/licenses/busybox \
    && install -s -m 0755 busybox /opt/out/bin/ && cp busybox.links /opt/out/share/busybox/ \
    && cp LICENSE /opt/out/share/licenses/busybox/ \
    && cd / && rm -rf /tmp/busybox*

# wpa_supplicant 2.12 (BSD-3-Clause), the Wi-Fi client: upstream release, signature checked
# 2026-10-19 against Jouni Malinen's key EC4AA0A991A5F2464582D52D2B6EF432EFC895FA. Android's build on
# the headset can't be used: it needs Android's HIDL service manager, which needs Android's properties.
COPY wpa_supplicant.config /tmp/wpa.config
RUN curl -fsSL -o /tmp/wpa.tar.gz https://w1.fi/releases/wpa_supplicant-2.12.tar.gz \
    && echo "08e23937e16d0155e55cab2b51f51fbe10d80a1aa91c4e15442645059b737ef6  /tmp/wpa.tar.gz" | sha256sum -c - \
    && tar -xzf /tmp/wpa.tar.gz -C /tmp && cd /tmp/wpa_supplicant-2.12/wpa_supplicant \
    && cp /tmp/wpa.config .config \
    && make -j"$(nproc)" LDFLAGS=-static wpa_supplicant wpa_cli > /dev/null \
    && mkdir -p /opt/out/bin /opt/out/share/licenses/wpa_supplicant \
    && install -s -m 0755 wpa_supplicant wpa_cli /opt/out/bin/ \
    && cp ../COPYING ../README /opt/out/share/licenses/wpa_supplicant/ \
    && cd / && rm -rf /tmp/wpa*

# Licences of the libraries linked in, from each project at the version Alpine installed
RUN v() { apk list -I "$1" | sed -E "s/^$1-([0-9.]+)-r.*/\1/"; } \
    && L=/opt/out/share/licenses && mkdir -p $L/musl $L/libnl $L/openssl \
    && curl -fsSL -o $L/musl/COPYRIGHT "https://git.musl-libc.org/cgit/musl/plain/COPYRIGHT?h=v$(v musl-dev)" \
    && curl -fsSL -o $L/libnl/COPYING "https://raw.githubusercontent.com/thom311/libnl/libnl$(v libnl3-static | tr . _)/COPYING" \
    && curl -fsSL -o $L/openssl/LICENSE.txt "https://raw.githubusercontent.com/openssl/openssl/openssl-$(v openssl-libs-static)/LICENSE.txt"

CMD ["sh", "-c", "mkdir -p /work/out/third_party && cp -a /opt/out/. /work/out/third_party/ && ls -l /work/out/third_party/bin"]
