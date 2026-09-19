---
name: jade-diy-fw
description: Maintain this Blockstream Jade fork for DIY hardware - merge/update upstream Jade commits while preserving the local T-Display S3 Pro customizations, then build and flash. Use when asked to merge or update upstream Jade, rebuild the DIY firmware, flash the LilyGO T-Display S3 Pro with OV5640 camera (tdisplays3pro / TTGO_TDISPLAYS3PROCAMERA), or tune camera live preview refresh or screen brightness.
---

# Jade DIY firmware (T-Display S3 Pro / OV5640)

Fork of Blockstream/Jade carrying local changes for a LilyGO **T-Display S3 Pro
with OV5640 camera shield**. This skill is the checklist for keeping the fork in
sync with upstream and shipping a working image, without re-breaking the
hardware quirks documented below.

## Repository facts

- Branch: `tdisplays3pro-ov5640`; `origin` = `https://github.com/Blockstream/Jade.git`
  (upstream development is on GitLab, GitHub is the mirror).
- Toolchain: **ESP-IDF v5.1.2** at `/Volumes/Crucial2T/Mac/leo_temp/lilygo/esp/esp-idf`
  (`source .../esp-idf/export.sh`).
- Python env for RPC verification: `/Users/kccleoc/.espressif/python_env/idf5.1_py3.12_env/bin/python`
  (has `cbor2`; `jadepy` needs it).
- `build_s3pro/` and `sdkconfig_tdisplays3pro` are local artifacts - **never commit them**.

## Target device (default)

`TTGO_TDISPLAYS3PROCAMERA`, built from `configs/sdkconfig_display_ttgo_tdisplays3procamera.defaults`:

| Item | Value |
| --- | --- |
| Panel | ST7796U SPI, 480x222 |
| Backlight | GPIO **48** (PWM dimmable) |
| On-board LED / camera fill light | GPIO **38** - do NOT drive as backlight |
| Camera | OV5640, SPI/I2C pins per Kconfig |
| Flash | 8MB setting works (device has 16MB) |

Do **not** build the non-Pro `BOARD_TYPE_TTGO_TDISPLAYS3` target for this
board: it uses a 320x170 i80 panel and drives GPIO 38 as "backlight", which
lights the camera fill light while leaving the screen black.

## Step 1 - Merge upstream

```bash
git remote -v                       # expect Blockstream/Jade
git fetch origin --prune
git status --short                  # working tree should be clean first
git merge origin/master
```

Merge is normally conflict-free; upstream has not touched the DIY board files.
If conflicts occur, keep the local version for the customized files below and
re-apply upstream logic around them.

After merging, verify the customizations still exist (`rg` each one), then
rebuild.

## Customization registry (protect these across merges)

| Feature | Files / values |
| --- | --- |
| Pro power + PWM backlight | `main/power/tdisplays3pro.inc`; routed in `main/power.c` |
| Camera XCLK 24 MHz | `main/Kconfig.projbuild` `CAMERA_XCLK_FREQ` default for PROCAMERA |
| Display SPI 40 MHz | `configs/sdkconfig_display_ttgo_tdisplays3procamera_camtweaks.defaults` |
| USB CDC reliability | `configs/sdkconfig_display_ttgo_tdisplays3_usbfix.defaults` (RX/TX 2048) |
| Brightness menu enabled | `main/ui/dashboard.c`, `main/process/dashboard.c`, `main/display.c` (default `BACKLIGHT_MEDIUM`) |
| QR passphrase scan | `main/process/mnemonic.c`, `main/ui/mnemonic.c`, `main/button_events.h` |
| Self-hosted blind oracle | `main/process/pinclient.c`, `pinserver_public_key.pub` |
| USB TX yield fix | `main/serial.c` |
| IDF 5.1.2 accommodations | `main/idf_component.yml`; vendored LCD moved to `factory/esp_lcd_v554/`; `bootloader_components_factory_multisig/`; stock bootloader, secure boot disabled for DIY |

### Adopted parameter values

- **Screen brightness** (`main/power/tdisplays3pro.inc`, LEDC PWM on
  `CONFIG_DISPLAY_PIN_BL` = GPIO48): levels map to
  `{25, 40, 50, 75, 100}` percent for `BACKLIGHT_MIN..MAX`; default is
  `BACKLIGHT_MEDIUM` = **50%** (previously the backlight was always full on).
- **Camera live preview refresh**: OV5640 XCLK **24 MHz** (was 16) and display
  SPI **40 MHz** (was 32). If preview is still sluggish, next levers are SPI
  80 MHz (ST7796 spec ~62.5 MHz, may glitch) and scanning quirc every other
  frame in `main/camera.c` / `main/qrscan.c`.
- **QR passphrase**: `get_freetext_passphrase()` offers `Scan QR` / `Keyboard`
  (camera builds only); scanned text is confirmed then falls back to keyboard
  on decline/abort.

## Step 2 - Build

```bash
source /Volumes/Crucial2T/Mac/leo_temp/lilygo/esp/esp-idf/export.sh
idf.py -B build_s3pro -DSDKCONFIG=sdkconfig_tdisplays3pro \
  "-DSDKCONFIG_DEFAULTS=configs/sdkconfig_display_ttgo_tdisplays3procamera.defaults;configs/sdkconfig_display_ttgo_tdisplays3_usbfix.defaults;configs/sdkconfig_display_ttgo_tdisplays3procamera_camtweaks.defaults" \
  build
```

Gotchas:

- `sdkconfig.defaults` **cannot override hidden Kconfig symbols** (e.g.
  `CAMERA_XCLK_FREQ` lives in the camera pin menu, not visible for this board) -
  edit the `default` in `main/Kconfig.projbuild` instead.
- Changing `SDKCONFIG_DEFAULTS` is not always picked up from cache: delete
  `sdkconfig_tdisplays3pro` (and `build_s3pro/CMakeCache.txt` if needed) to
  force regeneration, then confirm the values with `rg` in the generated
  `sdkconfig_tdisplays3pro`.

## Step 3 - Flash

Ask the user to enter download mode: **hold BOOT, tap RST, release BOOT**, then:

```bash
ls /dev/cu.usb*                                  # download: /dev/cu.usbmodem14101
source .../esp-idf/export.sh
idf.py -B build_s3pro -p /dev/cu.usbmodem14101 flash
```

After flashing, user releases BOOT and taps RST; the app then enumerates as
`/dev/cu.usbmodem1234561`.

## Step 4 - Verify

```bash
PY=/Users/kccleoc/.espressif/python_env/idf5.1_py3.12_env/bin/python
$PY - <<'EOF'
from jadepy import JadeAPI
j = JadeAPI.create_serial(device="/dev/cu.usbmodem1234561", timeout=5)
j.connect(); j.drain()
print("PING:", j.ping())
print(j.get_version_info())
j.disconnect()
EOF
```

Expect `PING: 0` and `BOARD_TYPE: TTGO_TDISPLAYS3PROCAMERA` (version ends
`-dirty` when built from a dirty tree). Note console logging is disabled
(`CONFIG_ESP_CONSOLE_NONE`/USJ), so there is no boot log to read.

## Commit and report

- Commit only when the user asks. Stage `main/**`, `configs/**`; leave
  `build_s3pro/` and `sdkconfig_tdisplays3pro` untracked.
- Commit style: `<board>: <summary>` then a bulleted body (see `git log`).
- Report the exact files changed, the parameter values used, and the flash
  result.

## Optional: parallelise with herdr

For a large upstream merge, a second pane running an agent can do the merge
while another implements/rebuilds. Verify the session first with
`test "${HERDR_ENV:-}" = 1`, then `herdr pane split --current --direction right --cwd "$PWD" --no-focus`
and `herdr agent start <name> --kind opencode --pane <id>`. Do not commit from
either agent without user approval.
