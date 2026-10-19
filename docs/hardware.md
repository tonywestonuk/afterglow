# Quest 1 hardware notes

Everything here was read from a retail Quest 1 (firmware `49845030443200410`) unless marked otherwise.

## Summary

| | |
|---|---|
| SoC | Qualcomm Snapdragon 835 (`msm8998`; the `apq8998` variant: its modem DSP has no cellular radio, but runs the Wi-Fi firmware) |
| Wi-Fi | Qualcomm WCN3990-family, `icnss` driver (firmware reports chip_id `0x30214`), 2.4 and 5 GHz, 802.11ac 2×2 |
| Board | `vs1 pvt b (SDC)` (Meta's codename for the headset is `monterey`) |
| RAM | 4 GB (3.9 GB visible) |
| Storage | UFS, six LUNs `sda`–`sdf`; 64 GB model has a 51 GB `userdata` |
| Displays | Two Samsung OLED panels (`sdc_lightman`), driven as one 2880×1600 surface at 72 Hz over split DSI |
| Firmware | Android 10, Linux `4.4.205-perf`, built with GCC 4.9 |
| Boot image | Header v0, page size 4096, gzip kernel with 14 appended device trees, gzip ramdisk |
| System | Legacy system-as-root (A/B, `skip_initramfs`); file-based encryption on `userdata` |

## Kernel source

Meta publishes the Quest 1 kernel at
[facebookincubator/oculus-linux-kernel, branch `oculus-quest-kernel-master`](https://github.com/facebookincubator/oculus-linux-kernel/tree/oculus-quest-kernel-master).
Board files are in `arch/arm64/boot/dts/oculus/`:

- `vs1-pvt-b-sdc.dts`: this board revision with Samsung panels
- `dsi-panel-sdc-lightman-video.dtsi`: the panel description and power-on command sequence

The bootloader selects one of 14 appended device trees (prototype → PVT, AUO or Samsung panels) and
passes the panel on the command line:

```
mdss_mdp.panel=1:dsi:0:qcom,mdss_dsi_sdc_lightman_video:config0:1:qcom,mdss_dsi_sdc_lightman_video:config0:cfg:split_dsi:panelid:00004000
androidboot.panel_type=1TJ350
```

The running kernel's full config is in [`kernel/configs/`](../kernel/configs/). Notable: `CONFIG_USB_CONFIGFS_NCM=y`
(USB networking), `CONFIG_FB=y`, `CONFIG_BUILD_ARM64_APPENDED_DTB_IMAGE=y`.

## Bootloader

`fastboot oem device-info` on an unlocked headset:

```
Device unlocked: true
Device critical unlocked: true
Secure boot: true
Linux console enabled: false
```

- `fastboot boot <image>` works (RAM only). `max-download-size` is 768 MB.
- The fastboot menu on the headset offers: Boot Device, Device Info, Factory Reset, Sideload Update.
  Move with the volume buttons, select with power.
- Anti-rollback index 1 and 2 are `1596585601` (2020-08-05). That is why 2020-era signed firmware is
  still accepted, and therefore why the unlock exploit works.
- `Linux console enabled` may enable a kernel debug console; not yet investigated.
- **Ramdisk size limit** (2026-10-19): an image whose ramdisk was 39.8 MiB was accepted (`Booting OKAY`)
  but never ran: the headset came straight back to the fastboot menu. 38.4 MiB had booted, and the same
  ramdisk gzipped (31.3 MiB) boots. The exact limit and its cause (probably the ramdisk overlapping
  something the bootloader placed after it) are unverified.
- **The bootloader stops answering after a failure.** After that failed boot, and after a `fastboot flash`
  whose transfer was aborted, the menu stayed on screen but fastboot commands hung, even after replugging.
  Holding power ~15 s and then Volume Down + Power fixed it every time.
- `fastboot flash system_b` of a 2.5 GB ext4 image (sent as a 1.4 MB sparse image) wrote the filesystem,
  but the transfer ended with `usb_read failed with status e00002eb` (I/O aborted) during the
  bootloader's write ([docs/storage.md](storage.md)).

## Display (framebuffer)

Under Meta's kernel the display is a classic Qualcomm MDSS framebuffer, `mdssfb_80000` (`/dev/graphics/fb0`
in Android; afterglow creates `/dev/fb0` from `/sys/class/graphics/fb0/dev`, as there is no devtmpfs):
2880×1600 at 72 Hz, 32 bpp, virtual 2880×3200 (two pages, so page-flipping with `FBIOPAN_DISPLAY`
works), stride 11520. A static program drawing to it shows on both panels with no Android running.

**The panels show the framebuffer rotated 180°**: viewer (x, y) is framebuffer (2879−x, 1599−y), so
the left half of the framebuffer is in front of the right eye. There is no orientation property in the
device tree; Meta's compositor compensates, and so does afterglow's boot animation player. (Confirmed
2026-10-18: an unrotated stereo animation appeared rotated, with correct depth, which only a full
180° rotation produces.)

Lens centres as the viewer sees the screen: about (852, 865) and (2028, 865), where Meta's splash logo
appears.

**The panels are also slightly rolled.** On the development headset, measured with afterglow's
calibration screen (2026-10-18), each eye's image needs turning **+1.0° (left) and +2.0° (right)**
counterclockwise to look level and line up. This varies per headset. Meta's factory calibration has a
`roll` per panel in the `private` partition (`calibration/display/<side>/<side>_screen_offset.json`;
here left 0.010, right 0.176, alongside `decenter` in pixels and mm and the panel serial), but its
units and sign are unconfirmed: read as radians it predicts a 10° difference between the eyes, which
a viewer doesn't see. Each panel also has a ~7 MB `.mura` uniformity correction in the same folder.

Pixel format: 32 bpp with red in bits 0–7, green 8–15, blue 16–23 (RGBA byte order).

## USB

One USB device controller, `a800000.dwc3` (Synopsys DWC3). Android configures it through configfs
(`androidboot.configfs=true`); afterglow does the same for its serial console. The stock kernel has
NCM, MTP, FunctionFS (adb) and Qualcomm functions but no ACM serial; afterglow's kernel adds it.

When the cable is pulled (or the Type-C link drops: seen once, `usbpd usbpd0: USB Type-C disconnect`),
the kernel hangs up the gadget's serial ports: reads return 0 and writes fail until the port is opened
again. afterglow's `/init` reopens its console port and restarts the shell, so both come back by
themselves (tested 2026-10-19 by detaching and reattaching the gadget's UDC).

## Buttons

| Device | Keys |
|---|---|
| `qpnp_pon` (power management chip) | Volume Down, Power |
| `gpio-keys` | Volume Up (plus `KEY_CAMERA_FOCUS` and code 0x2fe, unidentified) |

Holding Power for about 10 seconds forces a hardware restart.

`pstore`/ramoops (1 MB at `0x9d300000`) is configured, but its contents do not survive a restart into
the bootloader, so it can't be used to read a test boot's log afterwards.

## What the kernel brings up without Android

Surveyed from afterglow's shell (2026-10-18), with Meta's kernel config and no Android userspace:

| Hardware | State | Notes |
|---|---|---|
| CPU | Up | 8 cores: 4 at 1.9 GHz, 4 at 2.3 GHz |
| Display, buttons | Up | See above |
| Cameras | Driver loaded | `/dev/video0–6`, `video32/33`, 14 `v4l-subdev`s (Qualcomm camera stack) |
| GPU (Adreno 540) | Driver loaded | `kgsl-3d0`; needs firmware and the `a540_zap` subsystem (see below: now started) |
| Wi-Fi | Absent | No `wlan0`; needs firmware and the modem/WLAN subsystem (see below: now working) |
| Audio | Absent | No ALSA card; needs the `adsp` subsystem (see below: now started) |
| Bluetooth | Absent | Needs firmware over its serial link |
| Motion sensor | Not visible | Only the PMIC's battery ADC is under IIO; the IMU is probably behind a microcontroller Meta's userspace drives |

The Snapdragon's subsystems (`/sys/bus/msm_subsys`: `adsp`, `modem`, `venus`, `spss`, `ipa_fws`,
`a540_zap`) are all offline: Android's init starts them and loads their firmware. Memory in use with
afterglow's `/init`, BusyBox and the boot animation running: about 200 MB of 3.8 GB.

## Subsystems and Wi-Fi without Android

Confirmed 2026-10-19: `system/scripts/bringup.sh` starts the subsystems the way Android's init does
(`init.monterey.rc`), using firmware and Qualcomm's helper programs in place from the headset's own
`modem` and `system` partitions, mounted read-only. Anything they write goes to RAM.

| Subsystem | Started by | Result |
|---|---|---|
| Audio DSP (`adsp`) | `echo 1 > /sys/kernel/boot_adsp/boot` | Online; ALSA card `msm8998-cm710x_pvt_revB-snd_card`. Playback untested |
| GPU (`a540_zap`) | Opening `/dev/kgsl-3d0` | `Adreno540v2`, microcode and zap shader loaded. No userspace GPU driver yet |
| Modem DSP (`modem`) | Qualcomm's Peripheral Manager (`pm-service`, `pm-proxy`) | Stable. No cellular radio: it hosts the Wi-Fi firmware as a "user protection domain" (`wlan_pd`) |
| Wi-Fi | `echo 1 > /sys/kernel/boot_wlan/boot_wlan`, with `cnss-daemon` | `wlan0` 15 s after the kernel starts. Scans both bands; joins WPA2 networks; DHCP, DNS and HTTP work |

What the modem and Wi-Fi need, each found by comparing with stock Android (its kernel log, logcat
and IPC Router state, read as root):

1. **Firmware path.** The `modem` partition (FAT) mounted at `/vendor/firmware_mnt`, and
   `firmware_class.path=/vendor/firmware_mnt/image`.
2. **`restart_level=RELATED`** on each subsystem. The default restarts the whole headset when one crashes.
3. **The modem's settings filesystem (EFS)**, served by `rmt_storage` from `modemst1`, `modemst2`, `fsg`
   and `fsc`. `rmt_storage` writes to them, so afterglow gives it loop devices over RAM copies.
4. **`tftp_server`**, which serves the DSPs files from `/vendor/rfs`. The modem crashes if it can't
   write there (it first writes `readwrite/server_check.txt`); afterglow's tree has its writable
   folders in RAM, owned by `vendor_rfs` (2903) and group `vendor_rfs_shared` (2904).
5. **`pd-mapper`**, the service locator (QMI service `0x40`). Without it the Wi-Fi user-PD announces
   itself, nothing acknowledges it, and the modem crashes about 40 s later ("User-PD grace timer expired").
6. **IPC Router security rules**, loaded by `irsc_util /vendor/etc/sec_config`. Until they're loaded,
   the kernel only lets the `net_raw` group reach the DSPs' services. `tftp_server` drops root, so the
   modem's requests reach it but its replies are dropped, and the Wi-Fi firmware (`wlanmdsp.mbn`,
   3.2 MB) is never delivered.
7. **`cnss-daemon`** downloads the board data (`bdwlan.bin`) once the Wi-Fi firmware's QMI service is
   up; the firmware reports ready about 2.5 s later and the driver creates `wlan0`. It needs the
   folders under `/data/vendor/wifi` that Android's init makes. It also asks for `regdb.bin`, which
   isn't on the headset; that does no harm.
8. **Regulatory country.** The driver starts with US rules. `wpa_supplicant` sets the real country
   (`wifi.sh country GB`), which the kernel confirms ("Regulatory domain changed to country: GB").

Running Android's programs without Android:

- They run from the read-only `system` partition, bind-mounted at `/system` and `/vendor` (not
  symlinked: Android's linker picks its library namespace from a binary's real path), with the runtime
  APEX bind-mounted at `/apex/com.android.runtime`.
- There's no property service, so the linker can't choose its config (`ld.config.29.txt`) and falls
  back to `/system/lib64` and `/vendor/lib64`. Programs that need VNDK libraries (`cnss-daemon`) get
  `LD_LIBRARY_PATH=/system/lib64/vndk-29:/system/lib64/vndk-sp-29`.
- Several drop root after starting, so `/dev/null`, `/dev/urandom` and the binder nodes must be
  world-usable (0666), as Android's `ueventd.rc` makes them.
- Their logs go to `/dev/socket/logdw`; afterglow's `logdw` forwards them to the kernel log. Android's
  logging drops messages rather than wait, so the socket queue limit is raised to 600
  (`net.unix.max_dgram_qlen`), as Android's `init.rc` does.
- Android's own `wpa_supplicant` can't be used. It switches to the `wifi` user and registers with
  Android's HIDL service manager, which waits forever for the `hwservicemanager.ready` property.
  afterglow uses upstream `wpa_supplicant` 2.12 instead (`system/third_party`).

Boot timing with `/init` starting the bring-up (2026-10-19, from the kernel starting): Wi-Fi firmware
ready at 14 s; the kernel's random pool is seeded only at about 20 s (no hardware RNG driver is
registered: `rng_available` is empty), and `wpa_supplicant` waits for it; Wi-Fi address at 27 s.

Measured on a home network (2026-10-19): 21 networks in range across 2.4 and 5 GHz; joined at
WPA2-PSK on 5 GHz, link rate 866 Mbit/s at -40 dBm; a 64 MB HTTP download from a Mac took 10.2 s
(about 6.3 MB/s, served by Python's single-threaded `http.server`, so a lower bound). Ping to the
internet: 12–115 ms, probably raised by Wi-Fi power saving (untested).

## Boot screens

| Screen | Comes from | Changeable? |
|---|---|---|
| Unlocked-bootloader warning ("BOOT WILL RESUME SHORTLY. PRESS POWER KEY TO RESUME NOW.") | Text compiled into Meta's signed bootloader (`abl`, in an LZMA-compressed UEFI volume) | No: `abl` is signature-checked by the chip. Pressing power skips the wait |
| Still Oculus "O" logo | Two 8-bit BMPs in the `splash` partition behind a `DDPH` header: 1920×1600 at `0x4000`, 2880×1600 at `0xA00000` | Probably, by writing `splash` with a same-format image. Untested: unknown whether `abl` validates it |
| Animated Meta logo | Android's `bootanim` service running Meta's `/system/bin/vr_bootanimation` | Only by modifying `/system`. Under afterglow it doesn't exist |

## Slots

The headset has A/B slots. After an unlock with the web unlocker (and probably QuestStack):

- **Slot A** (active) is byte-identical to Meta's stock images for the current firmware. The stock
  bootloader honours the unlocked state.
- **Slot B** has been rewritten with a 2020-era Meta-signed boot chain (the exploitable bootloader), while
  `system_b` still holds an older 2024 Android. It is a mismatched slot: **do not `fastboot set_active b`**.

## Partitions

Per-slot (`_a` / `_b`): `xbl`, `abl`, `rpm`, `tz`, `hyp`, `devcfg`, `pmic`, `cmnlib`, `cmnlib64`,
`keymaster`, `ovrtz`, `modem`, `bluetooth`, `boot`, `system`.

Shared, and likely unique to each headset — **back up, never write**:

| Partition | Size | Probably |
|---|---|---|
| `persist` | 32 MB | Sensor and device calibration |
| `private` | 64 MB | Device-unique data |
| `vision` | 512 MB | Tracking calibration / data |
| `devinfo` | 4 KB | Unlock state |
| `modemst1`, `modemst2`, `fsg`, `fsc` | small | Radio calibration |

Other shared partitions: `ssd`, `misc`, `keystore`, `frp`, `cdt`, `ddr`, `sec`, `apdp`, `msadp`, `dip`,
`dpo`, `splash` (boot logo), `limits`, `toolsfv`, `logfs`, `sti`, `logdump`, `storsec`, `userdata`.
