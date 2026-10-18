# Quest 1 hardware notes

Everything here was read from a retail Quest 1 (firmware `49845030443200410`) unless marked otherwise.

## Summary

| | |
|---|---|
| SoC | Qualcomm Snapdragon 835 (`msm8998`; the modem-less `apq8998` variant) |
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

## Buttons

| Device | Keys |
|---|---|
| `qpnp_pon` (power management chip) | Volume Down, Power |
| `gpio-keys` | Volume Up (plus `KEY_CAMERA_FOCUS` and code 0x2fe, unidentified) |

Holding Power for about 10 seconds forces a hardware restart.

`pstore`/ramoops (1 MB at `0x9d300000`) is configured, but its contents do not survive a restart into
the bootloader, so it can't be used to read a test boot's log afterwards.

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
