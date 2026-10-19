#!/bin/sh
# Make afterglow's storage: format the spare slot's system partition as ext4 labelled "afterglow"
# (docs/storage.md). This ERASES that partition, so back it up first (tools/backup_partitions.sh)
# and make sure the spare slot holds nothing you need. It has to be named, as a confirmation:
#   sh /afterglow/format_storage.sh --erase system_b
set -eu
slot=$(sed -n 's/.*androidboot.slot_suffix=\([^ ]*\).*/\1/p' /proc/cmdline)
case $slot in
    _a) target=system_b ;;
    _b) target=system_a ;;
    *) echo "unknown active slot '$slot'"; exit 1 ;;
esac
if [ "${1:-}" != --erase ] || [ "${2:-}" != "$target" ]; then
    echo "usage: $0 --erase $target   (erases $target, the spare slot's system partition)"
    exit 1
fi

part=
for u in /sys/class/block/*/uevent; do
    if grep -qx "PARTNAME=$target" "$u"; then part=${u%/uevent}; fi
done
[ -n "$part" ] || { echo "no $target partition"; exit 1; }
dev=/dev/$target
[ -e "$dev" ] || mknod "$dev" b "$(cut -d: -f1 "$part/dev")" "$(cut -d: -f2 "$part/dev")"
if grep -qE "^($dev|/dev/${part##*/}) " /proc/mounts; then
    echo "$target is mounted"
    exit 1
fi
size=$(($(cat "$part/size") * 512))
echo "formatting $target (${part##*/}, $size bytes) as afterglow storage"
# Features Meta's 4.4 kernel can mount read-write: mke2fs 1.47 adds orphan_file by default (5.15+).
# Inode tables and journal written now, not lazily by the kernel after the first mount.
mke2fs -q -F -t ext4 -b 4096 -L afterglow -m 0 -O ^orphan_file,^metadata_csum_seed \
    -E lazy_itable_init=0,lazy_journal_init=0 "$dev"
sync
blkid "$dev"
