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
