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

- `origin` = `https://github.com/Blockstream/Jade.git` (upstream develops on GitLab;
  GitHub is the mirror). Local `master` is a stale mirror - do not build it.
- `fork` = `https://github.com/kccleoc/Jade.git` (personal backup). Push work here
  (`git push fork <branch>`); keep `origin` pointed at Blockstream for upstream
  merges. Branches track `fork`, so a bare `git push` goes to the backup, not upstream.
- **Integration branch: `tdisplays3pro-ov5640`** - the branch to build/flash. It
  carries the DIY board support plus everything in the registry below.
- **Topic branches** (see "Branch model and keeping local mods"):
  `feat/usb-host-storage`, `feat/taproot-keypath-taptree`,
  `fix/gui-split-varargs-abort`, `fix/pinserver-reply-timeout`.
- Toolchain: **ESP-IDF v5.1.2** at `/Volumes/Crucial2T/Mac/leo_temp/lilygo/esp/esp-idf`
  (`source .../esp-idf/export.sh`).
- **NEVER change the shared toolchain** - no `git checkout`/`switch_to.sh`/`install.sh`
  in the IDF repo, no other tag/branch. The fork is pinned to v5.1.2 while CI builds
  with v5.5.4. If code needs a newer IDF API, guard it with `ESP_IDF_VERSION` (see the
  SY6970 I2C code in `main/power/tdisplays3pro.inc`).
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
| USB host mass storage (SY6970 OTG) | `main/power/tdisplays3pro.inc` (version-conditional I2C), `main/camera.c` (shares the PMU bus), `main/power.h`, `main/Kconfig.projbuild` (`I2C_SDA=5` / `I2C_SCL=6`) |
| Taproot key-path of script-tree outputs | `main/utils/psbt.c` (`key_iter_is_supported_taproot` allows a merkle root AND leaf scripts (0x15) for a single-keypath key-path spend; still rejects >1 keypath and output taptrees); vectors `tests/rpc/data/sign_psbt/psbt_ss_p2tr_taptree_*` |
| Pinserver reply timeout | `main/process/pinclient.c`, `main/process.c`, `main/process.h` |
| GUI split varargs fix | `main/ui/qrmode.c`, `main/ui/dashboard.c` |
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

## Branch model and keeping local mods

Two tiers:

- `tdisplays3pro-ov5640` is the **integration** branch (board support + upstream
  merges). Build and flash from here.
- **Topic branches** isolate one change each, so upstream merges stay small and
  upstreamable fixes can be proposed back:

  | Branch | Base | Upstreamable? |
  | --- | --- | --- |
  | `feat/usb-host-storage` | board history | no (needs `tdisplays3pro.inc`) |
  | `feat/taproot-keypath-taptree` | `origin/master` | yes |
  | `fix/gui-split-varargs-abort` | `origin/master` | yes |
  | `fix/pinserver-reply-timeout` | `origin/master` | yes |

  Branches based on `origin/master` are keep-rebased and can be submitted as GitLab
  merge requests; once upstream takes one, delete it and just track `origin/master`.
  Board-coupled work stays on fork-based branches.

### Upstream update runbook

1. Working tree clean; `git switch tdisplays3pro-ov5640`.
2. `git fetch origin --prune && git merge origin/master`.
3. Resolve conflicts by keeping the registry version for customized files and
   re-applying upstream logic around them. Rebase the `origin/master`-based topic
   branches, then merge them into the integration branch.
4. **Re-verify every registry item** (`rg` file/line/string) before building -
   upstream edits to shared files (`main/utils/psbt.c`, `main/process.c`,
   `main/camera.c`, `main/ui/dashboard.c`) can silently drop or break a mod.
5. Rebuild (Step 2), run the RPC check (Step 4), and re-test on-device: PIN bind,
   USB storage (on battery + FAT32 drive), and taproot key-path.
6. CI gates that must stay green: `test_format` (needs `clang-format-19`; install
   with `pip install clang-format==19.1.7` if absent), `test_configs`, the DIY
   `build_diy_display_ttgo_tdisplays3procamera` compile, and `test_libjade*`
   (new JSON vectors are auto-collected).
7. Push the integration branch and any rebased topic branches to `fork`
   (`git push fork <branch>`); never push to `origin` (Blockstream).

### Guardrails for delegated agents

- Each agent gets a distinct file set; split builds across separate build dirs.
- Do not commit unless the user asks; never commit `build_s3pro/` or
  `sdkconfig_tdisplays3pro`.
- Do not switch, checkout, or reinstall the shared ESP-IDF (see Repository facts).

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
