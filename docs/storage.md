# afterglow's storage

Everything afterglow runs from is in RAM, so it needs somewhere to keep things across restarts: Wi-Fi
networks, the pairing with a computer, and later apps. That's `/storage`: an ext4 filesystem on the
**spare slot's system partition** (`system_b` when the headset runs from slot A), labelled `afterglow`.

## Why that partition

| Candidate | |
|---|---|
| **The spare slot's `system`** (chosen) | 2.5 GB. After an unlock with the web unlocker (or QuestStack), slot B holds a mismatched boot chain that must never be booted, and its `system` holds an old Android nothing uses. afterglow's plan is to own one slot anyway (Android in the other). Restorable exactly from a backup |
| `userdata` | 51 GB, but it's Android's live data (your apps and games), file-based encrypted, and too big to back up routinely |
| `misc`, `logfs`, `sti`… | Small, and used by the bootloader or Qualcomm's firmware |
| `persist`, `private`, `vision`, `modemst1/2`, `fsg`, `fsc`, `devinfo` | Per-device calibration and keys: **never** |

## What's kept there

| Path | What |
|---|---|
| `/storage/wifi/wpa_supplicant.conf` | Networks and the regulatory country. `tools/console.py --wifi` sends the WPA2 key derived from the password (PBKDF2, as every WPA2 device computes it), so the password itself is never stored; the key still lets someone join that network |
| `/storage/afterglowd/token` | The pairing token (`/run/afterglowd` links here) |

Mode 0700 directories, 0600 files. The bootloader is unlocked, so anyone holding the headset can read
them, as with most devices that remember Wi-Fi.

## Making it

Back up first (`tools/backup_partitions.sh`), and confirm the headset's spare `system` partition still
matches the backup, so it can be restored exactly. Then, on a headset running afterglow:

```sh
tools/console.py --run 'sh /afterglow/format_storage.sh --erase system_b'
```

The script picks the spare slot from the kernel command line, refuses anything else, refuses if the
partition is mounted, and formats it with features Meta's 4.4 kernel can mount read-write (current
`mke2fs` adds `orphan_file`, which needs 5.15). `bringup.sh storage` (part of `all`) then checks it
with `e2fsck -p` and mounts it at `/storage`. It only ever touches a partition labelled `afterglow`;
on a headset without one, settings stay in RAM.

Done on the reference headset 2026-10-19 (`system_b` = `sda7`, 2,684,354,560 bytes; it matched the
2026-10-18 backup byte for byte beforehand).

### Not with `fastboot flash`

The first attempt wrote an ext4 image with `fastboot flash system_b`. fastboot sent it as a 1.4 MB sparse
image, but the bootloader's write of the 2.5 GB it describes ended with the USB transfer aborted
(`usb_read failed with status e00002eb`), and the bootloader stopped responding until the headset was
power-cycled. The filesystem had in fact been written, but whether all of it was zeroed couldn't be
known, so it was formatted again from Linux. Formatting from afterglow needs no long transfer and
takes under a second.

## Undoing it

To put back what the spare slot had (it never boots anyway), restore it from the backup:

```sh
fastboot flash system_b backups/<date>/partitions/system_b.img
```

The bootloader still reports slot B as bootable (`slot-unbootable:b` is `no`), as before: its boot chain
was already mismatched, and its boot-chain partitions are untouched, so fastboot stays reachable either way.
