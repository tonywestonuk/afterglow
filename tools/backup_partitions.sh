#!/bin/bash
# Back up every partition (except userdata) of a rooted Quest 1 to the Mac, plus each
# UFS LUN's primary GPT, and verify every image against a sha256 computed on the headset.
# Re-running with the same <output-dir> skips partitions already verified OK.
# Usage: tools/backup_partitions.sh <output-dir>
set -u

# Prefer a current platform-tools in tools/; old adb/fastboot builds stall on large transfers
LOCAL_ADB="$(dirname "$0")/platform-tools-dl/platform-tools/adb"
[ -x "$LOCAL_ADB" ] && ADB="${ADB:-$LOCAL_ADB}" || ADB="${ADB:-adb}"
OUT="${1:?usage: $0 <output-dir>}"
SKIP="userdata"
STAGE="/data/local/tmp/afterglow-backup"

mkdir -p "$OUT/partitions" "$OUT/gpt"
MANIFEST="$OUT/manifest.tsv"
# Keep rows already verified OK from a previous (interrupted) run
if [ -f "$MANIFEST" ]; then
    awk -F'\t' '$5 == "OK"' "$MANIFEST" > "$MANIFEST.ok"
else
    : > "$MANIFEST.ok"
fi
printf "name\tdevice\tbytes\tsha256\tstatus\n" > "$MANIFEST"
cat "$MANIFEST.ok" >> "$MANIFEST"

root() { "$ADB" shell "su -c '$1'" | tr -d '\r'; }

# Quest suspends when not worn, which freezes the USB stream; hold a wake lock throughout
root "echo afterglow_backup > /sys/power/wake_lock"
trap 'root "rm -rf $STAGE; echo afterglow_backup > /sys/power/wake_unlock"' EXIT

"$ADB" shell getprop ro.build.fingerprint | tr -d '\r' > "$OUT/fingerprint.txt"
"$ADB" shell getprop ro.boot.slot_suffix | tr -d '\r' > "$OUT/slot.txt"

fail=0
for name in $(root "ls /dev/block/bootdevice/by-name"); do
    case " $SKIP " in *" $name "*) continue ;; esac
    if awk -F'\t' -v n="$name" '$1 == n { found = 1 } END { exit !found }' "$MANIFEST.ok"; then
        printf "%-14s already verified, skipping\n" "$name"
        continue
    fi
    dev=$(root "readlink /dev/block/bootdevice/by-name/$name")
    size=$(root "blockdev --getsize64 $dev")
    remote=$(root "sha256sum $dev" | cut -d' ' -f1)
    # Long `adb exec-out` streams stall partway through GB-sized partitions, so stage
    # each image in the headset's temp folder and fetch it with `adb pull` instead.
    root "mkdir -p $STAGE && dd if=$dev of=$STAGE/$name.img bs=1048576 2>/dev/null && chmod 644 $STAGE/$name.img"
    "$ADB" pull "$STAGE/$name.img" "$OUT/partitions/$name.img" > /dev/null
    root "rm -f $STAGE/$name.img"
    local_sum=$(shasum -a 256 "$OUT/partitions/$name.img" | cut -d' ' -f1)
    local_size=$(stat -f %z "$OUT/partitions/$name.img")
    if [ "$local_sum" = "$remote" ] && [ "$local_size" = "$size" ]; then status=OK; else status=MISMATCH; fail=1; fi
    printf "%s\t%s\t%s\t%s\t%s\n" "$name" "${dev#/dev/block/}" "$size" "$local_sum" "$status" >> "$MANIFEST"
    printf "%-14s %10s bytes  %s\n" "$name" "$size" "$status"
done

# Primary GPT of each LUN: protective MBR + header + entries (6 x 4096-byte blocks)
for lun in sda sdb sdc sdd sde sdf; do
    "$ADB" exec-out "su -c 'dd if=/dev/block/$lun bs=4096 count=6 2>/dev/null'" > "$OUT/gpt/$lun-primary-gpt.bin"
done

[ $fail -eq 0 ] && echo "ALL PARTITIONS VERIFIED" || echo "SOME PARTITIONS FAILED VERIFICATION"
exit $fail
