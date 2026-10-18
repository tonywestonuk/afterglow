# Temporary root and full backup

This gives stock Android root **for one boot only**, without writing to any partition, and uses it to
back up every partition to your computer. A normal restart returns the headset to stock, unrooted.

Tested on 2026-10-18 with firmware `49845030443200410`, Magisk v30.7 and platform-tools 37.0.1 on macOS.

## What you need

- An unlocked Quest 1 on firmware `49845030443200410`, with USB debugging working (`adb devices` lists it).
- **Current** [platform-tools](https://developer.android.com/tools/releases/platform-tools). Old builds
  (e.g. 29.x) stall forever at `Sending 'boot.img'`, which leaves the bootloader stuck until restarted.
- Meta's official full update for this firmware (a signed zip containing `payload.bin`) and
  [payload-dumper-go](https://github.com/ssut/payload-dumper-go).
- [Magisk](https://github.com/topjohnwu/Magisk/releases) v30.7 (check the release's sha256).

## 1. Extract the stock boot image

```sh
payload-dumper-go -o firmware/49845030443200410 -p boot <update>.zip
```

Check the header says patch level `2024-07`. (The inactive slot's `boot_b` is an older build and
must not be used.)

## 2. Patch it with Magisk, on the headset

`magiskboot` only runs on Android, so patching happens in `/data/local/tmp` (no partitions are touched).
From the Magisk APK take `assets/boot_patch.sh`, `assets/util_functions.sh`, `assets/stub.apk`, and from
`lib/arm64-v8a/` take `libmagiskboot.so`, `libmagiskinit.so`, `libmagisk.so`, `libinit-ld.so`, renamed to
`magiskboot`, `magiskinit`, `magisk`, `init-ld`. Push them with `boot.img` to `/data/local/tmp/magisk-patch/`, then:

```sh
adb shell 'cd /data/local/tmp/magisk-patch && KEEPVERITY=true KEEPFORCEENCRYPT=true \
  PATCHVBMETAFLAG=true RECOVERYMODE=false LEGACYSAR=true sh ./boot_patch.sh boot.img'
```

- `LEGACYSAR=true`: `/` is mounted from `/dev/root`. Look for
  `Patch ... [skip_initramfs] -> [want_initramfs]` in the output.
- `KEEPVERITY` and `KEEPFORCEENCRYPT`: what the Magisk app picks for an encrypted system-as-root device.
- The three `Failed to patch` lines are Samsung-only patches and are expected.

The result is `new-boot.img`.

## 3. Pre-approve root for the adb shell

Magisk asks for approval in a 2D window. On a Quest whose launcher has replaced Meta's home, that window
has nowhere to appear, and `su` waits forever. So add [`tools/root-overlay/`](../tools/root-overlay/) to the
ramdisk. At boot it allows root for uid 2000 (the adb shell) only:

```sh
./magiskboot unpack new-boot.img
./magiskboot cpio ramdisk.cpio \
  "add 0644 overlay.d/afterglow-grant-shell.rc afterglow-grant-shell.rc" \
  "add 0755 overlay.d/sbin/afterglow_grant_shell.sh afterglow_grant_shell.sh"
./magiskboot repack new-boot.img boot-root.img
```

(Tested with the same files under earlier names, `oculusos-*`.)

## 4. Boot it from RAM

```sh
adb install Magisk-v30.7.apk        # what we did; not yet known whether it's required
adb reboot bootloader
fastboot boot boot-root.img         # leave the headset's fastboot menu alone
adb shell su -c id                  # → uid=0(root) ... context=u:r:magisk:s0
```

If it doesn't come up, hold the power button 10–30 seconds: it restarts into stock Android.

## 5. Back up

```sh
tools/backup_partitions.sh backups/full-$(date +%F)
```

This copies all partitions except `userdata` (about 6 GB) plus each LUN's primary GPT, and compares a
sha256 taken on the headset with the copy on your computer. It holds a wake lock, because the Quest
suspends when not worn and that freezes transfers. It stages each image in `/data/local/tmp` and uses
`adb pull`, because a long `adb exec-out` stream stalls partway through the 2.5 GB system partitions. It
can be re-run safely: verified partitions are skipped.

Store the backup somewhere other than your computer too. It contains data unique to your headset.

## Afterwards

A normal restart removes root. To remove the leftovers in `userdata`:
`adb uninstall com.topjohnwu.magisk`, and (while rooted) `rm -rf /data/adb`.
