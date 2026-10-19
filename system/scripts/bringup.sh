#!/bin/sh
# Bring up the Snapdragon's subsystems the way Android's init does (init.monterey.rc), with firmware
# and Qualcomm's helper daemons used in place from the headset's own partitions, mounted read-only.
# Anything a daemon writes goes to RAM (/data is tmpfs) or fails against a read-only mount.
#
# Development helper for afterglow's USB shell:  sh /afterglow/bringup.sh STEP...
#   mounts   modem (FAT) at /vendor/firmware_mnt, system at /android, private at /persist
#   android  paths Android binaries expect (/system, /apex, /vendor/*), /data in RAM
#   storage  afterglow's own storage at /storage (kept across restarts), if the headset has it
#   ssr      a crashing subsystem restarts on its own instead of restarting the headset
#   adsp     audio DSP (sound card)
#   gpu      Adreno 540 (microcode and zap shader load on first open), ready for Qualcomm's GL driver
#   cnss     Qualcomm's cnss-daemon, which downloads the Wi-Fi board data once its firmware is up
#   efs      Qualcomm's rmt_storage on RAM copies of the modem's settings partitions
#   tftp     Qualcomm's tftp_server, which serves the DSPs their files (incl. Wi-Fi firmware)
#   pdmapper Qualcomm's protection domain mapper (service locator; the Wi-Fi user-PD needs it)
#   permgr   Qualcomm's Peripheral Manager, which boots the modem
#   modem    (alternative to permgr) hold the modem on by opening its device
#   wlan     start the Wi-Fi driver
#   wifi     wpa_supplicant and DHCP on wlan0 (join a network with wifi.sh)
#   afterglowd  the MCP server, for Claude Code on a computer (pair with tools/console.py --pair)
#   all      every step in order
set -u

slot=$(sed -n 's/.*androidboot.slot_suffix=\([^ ]*\).*/\1/p' /proc/cmdline)
log() { echo "bringup: $*"; echo "bringup: $*" > /dev/kmsg; }
# A line on the headset's status screen (system/init/status.c): status FILE LABEL TEXT
status() { mkdir -p /run/afterglow/status && printf '%s\t%s\n' "$2" "$3" > "/run/afterglow/status/$1"; }

# Block device node for a partition, found by its GPT name
part_node() {
    [ -e "/dev/$1" ] && return 0
    for u in /sys/class/block/*/uevent; do
        if grep -qx "PARTNAME=$1" "$u"; then
            mm=$(cat "${u%/uevent}/dev")
            mknod "/dev/$1" b "${mm%:*}" "${mm#*:}"
            return 0
        fi
    done
    log "no partition $1"
    return 1
}

# Run an Android daemon the way Android's init does: detached, and restarted whenever it exits
daemon() {
    name=$1
    shift
    setsid sh -c "while true; do $*; echo \"exited \$?; restarting\"; sleep 1; done" \
        < /dev/null >> "/tmp/$name.log" 2>&1 &
    log "$name started (log /tmp/$name.log)"
}

# Character device node from a sysfs dev file
char_node() {
    [ -e "$2" ] && return 0
    mm=$(cat "$1") && mknod "$2" c "${mm%:*}" "${mm#*:}"
}

mounted() { grep -q " $1 " /proc/mounts; }

# System at /android with its system/ and vendor/ bind-mounted at /system and /vendor (bind mounts,
# not symlinks: Android's linker picks its library namespace from the binary's real path), the
# modem partition's firmware at /vendor/firmware_mnt (an empty directory in the system image), and
# private at /persist. All read-only.
mounts() {
    part_node "modem$slot" && part_node "system$slot" && part_node private || return 1
    mkdir -p /android /system /vendor /persist /lib
    mounted /android || mount -t ext4 -o ro,noload "/dev/system$slot" /android || return 1
    mounted /system || mount --bind /android/system /system || return 1
    mounted /vendor || mount --bind /android/system/vendor /vendor || return 1
    mounted /vendor/firmware_mnt ||
        mount -t vfat -o ro,shortname=lower "/dev/modem$slot" /vendor/firmware_mnt || return 1
    mounted /persist || mount -t ext4 -o ro,noload /dev/private /persist || return 1
    [ -e /lib/firmware ] || ln -s /vendor/firmware /lib/firmware
    echo -n /vendor/firmware_mnt/image > /sys/module/firmware_class/parameters/path
    echo 5 > /sys/class/firmware/timeout   # don't wait 60 s for files that aren't there
    log "firmware partitions mounted read-only (slot $slot)"
}

android() {
    mkdir -p /apex/com.android.runtime /data
    mounted /apex/com.android.runtime ||
        mount --bind /android/system/apex/com.android.runtime.release /apex/com.android.runtime
    mounted /data || mount -t tmpfs -o mode=0771 tmpfs /data
    mkdir -p /data/vendor/tombstones/rfs/modem /data/vendor/tombstones/rfs/lpass
    # Wi-Fi's runtime folders, as Android's init.monterey.rc makes them (owner wifi, 1010)
    mkdir -p /data/vendor/wifi/sockets /data/vendor/wifi/hostapd/ctrl /data/vendor/wifi/wpa_supplicant \
             /data/vendor/wifi/wpa/sockets
    chown -R 1010:1010 /data/vendor/wifi
    chmod -R 0770 /data/vendor/wifi
    rfs
    # Android daemons' log messages, forwarded to the kernel log (see system/tools/logdw.c). Android's
    # logging drops messages rather than wait, and Linux queues only 10 datagrams on a socket by
    # default; Android's init.rc raises that to 600.
    echo 600 > /proc/sys/net/unix/max_dgram_qlen
    [ -S /dev/socket/logdw ] || { setsid logdw < /dev/null > /tmp/logdw.log 2>&1 & sleep 0.2; }
    # IPC Router security rules, as Android's vendor.irsc_util does. Until they're loaded the kernel's
    # default rule lets only the net_raw group reach the DSPs' services, so daemons that drop root
    # (tftp_server) can't answer the modem, and the modem's Wi-Fi user-PD never gets its files.
    /vendor/bin/irsc_util /vendor/etc/sec_config && log "IPC Router security rules loaded"
    log "android paths ready (/data is RAM)"
}

# afterglow's own storage, kept across restarts: the spare slot's system partition, formatted ext4 with
# the label "afterglow" by format_storage.sh (docs/storage.md). It's used only if it carries that label,
# so a headset whose spare slot still holds Android is never touched; then everything stays in RAM.
# Checked with e2fsck first, as any Linux system checks its filesystems.
storage() {
    mounted /storage && return 0
    other=$([ "$slot" = _a ] && echo _b || echo _a)
    part_node "system$other" || return 1
    if ! blkid "/dev/system$other" | grep -q 'LABEL="afterglow"'; then
        log "no afterglow storage (system$other isn't labelled afterglow): settings stay in RAM"
        return 0
    fi
    e2fsck -p "/dev/system$other" > /tmp/e2fsck.log 2>&1
    rc=$?
    if [ $rc -gt 2 ]; then   # 1, 2: problems found and fixed
        log "storage: e2fsck couldn't fix system$other by itself (exit $rc, /tmp/e2fsck.log): not mounted"
        return 0
    fi
    mkdir -p /storage
    mount -t ext4 -o noatime,nodev,nosuid "/dev/system$other" /storage || return 1
    log "storage: system$other at /storage, $(df -h /storage | awk 'NR == 2 { print $4 }') free"
}

# The tree tftp_server serves the DSPs. Android's points their writable folders at /persist, which is
# read-only here, and the modem crashes when it can't write (it first writes readwrite/server_check.txt).
# This copy keeps the read-only paths and puts the writable ones in RAM (they're empty on the device
# anyway), owned by the user tftp_server switches to: vendor_rfs (2903), group vendor_rfs_shared (2904).
rfs() {
    mounted /vendor/rfs && return 0
    root=/data/vendor/rfs-tree
    for dsp in mpss adsp; do
        dumps=$([ $dsp = mpss ] && echo modem || echo lpass)
        mkdir -p $root/msm/$dsp/readonly/vendor /data/vendor/rfs/msm/$dsp
        ln -s /vendor/firmware_mnt $root/msm/$dsp/readonly/firmware
        ln -s /vendor/firmware $root/msm/$dsp/readonly/vendor/firmware
        ln -s /data/vendor/rfs/msm/$dsp $root/msm/$dsp/readwrite
        ln -s /data/vendor/rfs/shared $root/msm/$dsp/shared
        ln -s /data/vendor/rfs/hlos $root/msm/$dsp/hlos
        ln -s /data/vendor/tombstones/rfs/$dumps $root/msm/$dsp/ramdumps
    done
    mkdir -p /data/vendor/rfs/shared /data/vendor/rfs/hlos
    chown -R 2903:2904 /data/vendor/rfs /data/vendor/tombstones/rfs
    chmod -R 0770 /data/vendor/rfs /data/vendor/tombstones/rfs
    mount --bind $root /vendor/rfs   # over the system image's own /vendor/rfs
}

ssr() {
    for d in /sys/bus/msm_subsys/devices/*; do
        echo RELATED > "$d/restart_level" 2>/dev/null
    done
    log "subsystem restart level: $(cat /sys/bus/msm_subsys/devices/*/restart_level | sort -u | tr '\n' ' ')"
}

adsp() {
    echo 1 > /sys/kernel/boot_adsp/boot
    log "audio DSP starting"
}

gpu() {
    char_node /sys/class/kgsl/kgsl-3d0/dev /dev/kgsl-3d0 && (exec 3<>/dev/kgsl-3d0) || return 1
    log "GPU opened: $(cat /sys/class/kgsl/kgsl-3d0/gpu_model)"
    # For Qualcomm's OpenGL ES driver (system/gpu): the ION memory allocator, and Qualcomm's gralloc where
    # Android's HAL loader looks when there are no system properties to name it (gralloc.default.so).
    # A bind mount in RAM: the partition isn't changed.
    char_node /sys/class/misc/ion/dev /dev/ion
    mounted /vendor/lib64/hw/gralloc.default.so ||
        mount --bind /vendor/lib64/hw/gralloc.msm8998.so /vendor/lib64/hw/gralloc.default.so
}

# Qualcomm's protection domain mapper: hosts the service locator (QMI service 0x40), built from the
# DSPs' domain lists (*.jsn in the modem partition). Without it the Wi-Fi user-PD on the modem
# announces itself, nothing acknowledges it, and the modem crashes ~40 s later.
pdmapper() {
    daemon pd-mapper /vendor/bin/pd-mapper
}

# Android's linker picks its library config (ld.config.29.txt) from system properties, which don't
# exist here, so it falls back to /system/lib64 and /vendor/lib64; cnss-daemon also needs the VNDK.
cnss() {
    daemon cnss-daemon env LD_LIBRARY_PATH=/system/lib64/vndk-29:/system/lib64/vndk-sp-29 \
        /vendor/bin/cnss-daemon -n -l
}

# Qualcomm's Peripheral Manager, as in Android: boots the modem when pm-proxy votes for it. Both use
# binder, via vndservicemanager.
permgr() {
    for b in binder hwbinder vndbinder; do   # 0666, as Android's ueventd.rc has them
        [ -e "/sys/class/misc/$b/dev" ] && char_node "/sys/class/misc/$b/dev" "/dev/$b" && chmod 0666 "/dev/$b"
    done
    for s in /sys/class/subsys/subsys_*; do   # /dev/subsys_modem etc. (Android's ueventd makes these)
        char_node "$s/dev" "/dev/${s##*/}"
    done
    daemon vndservicemanager /vendor/bin/vndservicemanager /dev/vndbinder
    sleep 1
    daemon pm-service /vendor/bin/pm-service
    sleep 1
    daemon pm-proxy /vendor/bin/pm-proxy
}

modem() {
    char_node /sys/class/subsys/subsys_modem/dev /dev/subsys_modem || return 1
    # Holding the device open keeps the modem running (Android's Peripheral Manager does this)
    (exec 3<>/dev/subsys_modem; exec sleep 2147483647) </dev/null >/dev/null 2>&1 &
    log "modem starting"
}

tftp() {
    daemon tftp_server /vendor/bin/tftp_server
}

# The modem keeps its settings filesystem (EFS) on modemst1, modemst2, fsg and fsc, reached only
# through Qualcomm's rmt_storage, and crashes about 40 s after starting without it. rmt_storage
# writes to those partitions, so it gets RAM copies instead: loop devices over files in /data (RAM),
# linked where it looks for partitions. The real partitions are only ever read.
efs() {
    mkdir -p /data/vendor/efs /dev/block/bootdevice/by-name
    n=0
    for p in modemst1 modemst2 fsg fsc; do
        part_node $p || return 1
        [ -e /dev/loop$n ] || mknod /dev/loop$n b 7 $n
        if [ ! -e /dev/block/bootdevice/by-name/$p ]; then
            dd if=/dev/$p of=/data/vendor/efs/$p.img bs=65536 2>/dev/null &&
                losetup /dev/loop$n /data/vendor/efs/$p.img &&
                ln -s /dev/loop$n /dev/block/bootdevice/by-name/$p || return 1
        fi
        n=$((n + 1))
    done
    for u in /sys/class/uio/uio*; do
        char_node "$u/dev" "/dev/${u##*/}"
    done
    daemon rmt_storage /vendor/bin/rmt_storage
    log "rmt_storage serves RAM copies of modemst1/modemst2/fsg/fsc"
}

wlan() {
    echo 1 > /sys/kernel/boot_wlan/boot_wlan
    log "Wi-Fi driver starting"
}

# afterglow's own Wi-Fi client: upstream wpa_supplicant (system/third_party; Android's needs Android's
# service managers) and BusyBox's DHCP client. Networks are added with wifi.sh and kept in /storage
# (or in RAM, without storage), so the headset reconnects by itself after a restart.
wifi() {
    i=0
    until [ -e /sys/class/net/wlan0 ]; do   # the driver makes it once the firmware is ready
        i=$((i + 1))
        [ $i -le 30 ] || { log "no wlan0"; return 1; }
        sleep 1
    done
    mkdir -p /var/run /etc
    conf=/tmp/wpa_supplicant.conf
    if mounted /storage; then
        mkdir -p /storage/wifi && chmod 0700 /storage/wifi
        conf=/storage/wifi/wpa_supplicant.conf
    fi
    [ -e $conf ] || printf 'ctrl_interface=/var/run/wpa_supplicant\nupdate_config=1\n' > $conf
    chmod 0600 $conf
    if grep -q "network={" $conf; then
        status 20-wifi Wi-Fi "connecting…"
    else
        status 20-wifi Wi-Fi "no network saved"
    fi
    ip link set wlan0 up
    daemon wpa_supplicant /bin/wpa_supplicant -i wlan0 -D nl80211 -c $conf
    daemon udhcpc udhcpc -f -i wlan0 -A 3 -x hostname:afterglow -s /afterglow/udhcpc.sh
    # So wifi.sh works as soon as this returns. Early in a boot that can take a while: wpa_supplicant
    # first waits for the kernel's random pool, which is seeded about 20 s after boot (no hardware RNG)
    i=0
    until [ -S /var/run/wpa_supplicant/wlan0 ]; do
        i=$((i + 1))
        [ $i -le 300 ] || { log "wpa_supplicant not ready after 30 s"; return 1; }
        sleep 0.1
    done
}

# afterglow's MCP server (system/afterglowd), on port 7070 and as afterglow.local. It answers only
# requests carrying the token tools/console.py --pair puts in /run/afterglowd over USB; with storage,
# that's a link to /storage/afterglowd, so the pairing survives restarts.
afterglowd() {
    mkdir -p /run
    if mounted /storage; then
        mkdir -p /storage/afterglowd && chmod 0700 /storage/afterglowd
        [ -e /run/afterglowd ] || ln -s /storage/afterglowd /run/afterglowd
    else
        mkdir -p /run/afterglowd && chmod 0700 /run/afterglowd
    fi
    daemon afterglowd /bin/afterglowd
}

[ $# -gt 0 ] || { sed -n '6,22p' "$0"; exit 1; }
all=
for step in "$@"; do
    [ "$step" = all ] && all=1 &&
        set -- mounts android storage ssr adsp gpu efs tftp pdmapper cnss permgr wlan wifi afterglowd
done
# A failed step doesn't stop the rest: with afterglowd still running, the headset can be reached to
# find out why
[ -z "$all" ] || status 10-system System "starting…"
failed=
for step in "$@"; do
    case $step in
        mounts | android | storage | ssr | adsp | gpu | efs | tftp | pdmapper | cnss | permgr | modem | wlan | \
            wifi | afterglowd)
            $step || { log "$step failed"; failed="$failed $step"; } ;;
        *) echo "unknown step: $step"; exit 1 ;;
    esac
done
if [ -n "$failed" ]; then
    [ -z "$all" ] || status 10-system System "failed:$failed"
    exit 1
fi
[ -z "$all" ] || status 10-system System ready
