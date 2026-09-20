# USB Host Mass Storage — T-Display S3 Pro (DIY fork)

Status: plan (read-only investigation; no source/build changes)
Board: `TTGO_TDISPLAYS3PROCAMERA` (LilyGO T-Display S3 Pro, ESP32-S3R8, SY6970 PMU)
Owner: USB/firmware design
Cross-refs:
- `docs/plans/ci-and-test-strategy.md` §3 — CI limits / QA checklist for this feature
- `main/power/tdisplays3pro.inc` — file to change (primary)
- `main/power/jadev20.inc`, `main/power/tdisplays3.inc` — reference implementations
- `main/power/i2c.inc` — shared I2C helper library for PMU boards

---

## 1. Summary and user-visible outcome

The board currently cannot use USB mass storage at all. The USB-host stack, the
`usb_host_msc` driver, FATFS, the "USB Storage" menu and the three actions are
already compiled in; the missing pieces are the board's power-management hooks in
`main/power/tdisplays3pro.inc`, which are stubs (`:83-95`).

This plan makes the T-Display S3 Pro able to **host a USB drive / SD-card reader
plugged into its USB-C port** and use it exactly like the official Jade Plus:

- **Firmware Upgrade** — pick a compressed `*_fw.bin` + `.hash` from the drive.
- **Sign PSBT** — pick a `.psbt`, sign, write back `*_signed.psbt`.
- **Export Xpub** — write `jade-xpub.txt` (descriptor) to the drive.

Concretely, the fix is:

1. Enable the SY6970's 5 V **OTG boost** so the board can source VBUS to the
   attached drive (`enable_usb_host()` / `disable_usb_host()`).
2. Implement `usb_is_powered()` **truthfully** (read the SY6970 bus status) so the
   "disconnect USB power, then connect a storage device" gate and the host-start
   assertion work.

> **Consequence (not extra scope):** implementing `usb_is_powered()` correctly
> unavoidably fixes three side effects of the hard-coded `return true;`
> (`main/power/tdisplays3pro.inc:95`):
> - idle timeout now powers the device off when on battery
>   (`main/idletimer.c:136`, `action = !usb_is_powered() ? POWER_OFF : ...`),
> - USB-disconnect detection starts working (`main/process.c:418`,
>   `main/process/dashboard.c:2702`),
> - the status-bar `C`/`D` indicator starts reflecting reality
>   (`main/gui.c:2312`).
> These are the same behaviours every other battery Jade already has; they are
> listed here so they are expected and tested, not "surprises".

Out of scope (not planned): onboard microSD, USB HID, device-side MSC/DFU, and
"fix `usb_is_powered()` side effects" as a standalone item.

---

## 2. Current state and root cause

### 2.1 The stubs

`main/power/tdisplays3pro.inc`:

- `power_init()` (`:33-37`) only initialises the LEDC backlight. It does **no**
  I2C/PMU init — the file does not even include `power/i2c.inc` (`:1-16`).
- `enable_usb_host()` / `disable_usb_host()` are empty (`:92-93`).
- `usb_is_powered()` returns `true` unconditionally (`:95`).
- Battery/voltage accessors are stubs (`:83-90`).
- The header comment explicitly says "The board's SY6970 PMIC is not supported"
  (`:2-3`).

The board is routed to this file in `main/power.c:33-35`.

### 2.2 Why that breaks USB storage

The USB-storage entry point `handle_usbstorage_action()`
(`main/usbhmsc/usbmode.c:276-371`) does:

```c
while (usb_is_powered()) {                 // usbmode.c:283-288
    ... "Disconnect USB power and connect a storage device" ...
}
...
serial_stop();                             // usbmode.c:292
usbstorage_start();                        // usbmode.c:294
```

With `usb_is_powered() == true` always, the `while` loop **never exits**, so the
action hangs forever on the prompt. Even if it exited, `usbstorage_start()`
asserts:

```c
JADE_ASSERT(!usb_is_powered());            // usbhmsc.c:256
```

which would fire immediately (assert fails because `usb_is_powered()` is true).
And even if host mode started, `enable_usb_host()` is a no-op, so the SY6970 never
supplies 5 V to the drive (`usbstorage_impl()` calls `enable_usb_host()` at
`usbhmsc.c:104-106`, teardown at `:225-226`).

So there are three independent blockers, all in this one file: the wait loop, the
assert, and the missing OTG boost.

### 2.3 What is already in place (no work needed)

| Piece | Location |
|---|---|
| Host stack + `usbhmsc` driver sources compiled for S3 | `main/CMakeLists.txt:25-27, 39-56`; component `espressif/usb_host_msc ==1.1.3` (`main/idf_component.yml:23-26`) |
| `usbstorage_init()` called once at boot | `main/main.c:233-235` (`#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_HAS_BATTERY`) |
| USB Storage menu | `main/ui/dashboard.c:284-286, 313-315, 333-348` |
| Action dispatch (FW / Sign / Export Xpub) | `main/process/dashboard.c:2372-2402` |
| Actions themselves (FATFS/mount point `/usb`) | `main/usbhmsc/usbmode.c`, `main/usbhmsc/usbhmsc.h` |
| FATFS with LFN + 1 volume (needed for FAT32 + long names) | board defaults `configs/sdkconfig_display_ttgo_tdisplays3procamera.defaults:48-50`; generated `sdkconfig_tdisplays3pro:1381-1384` |
| CDC serial + start/stop (role switch) | `main/serial.c:208-334` |

### 2.4 Reference implementations

- `main/power/tdisplays3.inc:179-191` — `usb_is_powered()` via TinyUSB
  `tud_mounted() && !tud_suspended()` plus a `power_get_vbat() < 1000` fallback
  (the T-Display S3 has **no PMU**, so it cannot see the VBUS/role state).
- `main/power/jadev20.inc:275-287` — `enable_usb_host()` / `disable_usb_host()`
  write the STM32 `STM32_REG_OTG` register (`:27`) 1/0, under the I2C mutex.
- `main/power/jadev20.inc:289-300` — `usb_is_powered()` reads a dedicated role
  switch (`SGM7220`) and returns `true` only when connected as a USB **sink**.
- `main/power/i2c.inc` — `_power_i2c_init()` (`:60-72`) creates an `i2c_master`
  bus on `I2C_NUM_0` using `CONFIG_I2C_SDA` / `CONFIG_I2C_SCL`;
  `_power_i2c_attach_device()` (`:76-88`) attaches a device;
  `_power_write_command()` (`:111-114`) and `_power_master_read_slave()`
  (`:101-109`) are the helpers. This is what the SY6970 driver will reuse.

### 2.5 Board facts (verified against LilyGO/Xinyuan sources)

- **SY6970 PMU at 7-bit I2C `0x6A`**, on the **shared** I2C bus
  `SDA = GPIO5`, `SCL = GPIO6` — the same bus as the camera SCCB, LTR553 and
  CST816S (LilyGO T-Display-S3-Pro `examples/USB_HID_Example/utilities.h`:
  `BOARD_I2C_SDA 5`, `BOARD_I2C_SCL 6`; README I2C table).
- This matches the Jade camera pins already configured:
  `CONFIG_CAMERA_SDA=5`, `CONFIG_CAMERA_SCL=6`
  (`main/Kconfig.projbuild:453-466`).
- **OTG is enabled by setting REG03 bit5** (`OTG_CONFIG`); XPowersLib
  `PowersSY6970::enableOTG()` sets `REG_03H` bit5 and refuses if
  `isVbusIn()`; `disableOTG()` clears bit5 (LilyGO's `USB_HID_Example.ino`
  drives host power with exactly `PMU.enableOTG()`).
- Datasheet: REG03 bit5 = `OTG_CONFIG` (0 = OTG disable, 1 = enable);
  REG0A bits7:4 = `BOOSTV` (default 4.998 V, ~5 V); REG0A bits1:0 = boost current
  limit; REG0B[7:5] = `BUS_STAT` (0 no input … 7 OTG); REG0E = battery voltage
  (base 2304 mV, 20 mV steps); REG11 = VBUS voltage (base 2600 mV, 100 mV steps).
  (SY6970 datasheet via LilyGO wiki / `AN_SY6970.pdf`; register map mirrors the
  ESPHome `sy6970` component and XPowersLib.)
- Datasheet note: "Boost mode is enabled when REG03[5]=1 **and OTG pin is
  high**." LilyGO's headers expose **no OTG-enable GPIO** and their example only
  writes the register bit, so the OTG pin is presumed strapped high on the PCB.
  This is an open verification item (§7).

### 2.6 The I2C sharing problem (important)

The camera driver (`espressif__esp32-camera ==2.0.15`) does **not** share the PMU
bus today:

- `main/camera.c:270-271` passes `.pin_sscb_sda = CONFIG_CAMERA_SDA` /
  `.pin_sscb_scl = CONFIG_CAMERA_SCL` (GPIO5/6).
- Because `pin_sccb_sda != -1`, `camera_probe()` calls `SCCB_Init()`
  (`managed_components/espressif__esp32-camera/driver/esp_camera.c:172-178`),
  which unconditionally creates a **new** `i2c_master` bus on
  `SCCB_I2C_PORT_DEFAULT` (`driver/sccb-ng.c:114-142`).
- That port defaults to **I2C1** (`.../esp32-camera/Kconfig:127-136`; confirmed
  in `sdkconfig_tdisplays3pro:2126-2127`).

If the PMU simply calls `_power_i2c_init()` (which hard-codes `I2C_NUM_0`,
`main/power/i2c.inc:65`) with the camera pins, we get **two I2C master
controllers driving the same two wires** (PMU on port 0, camera on port 1). That
is a real concurrency hazard because `usb_is_powered()` is polled from the GUI/idle
tasks (`main/gui.c:2312`, `main/idletimer.c:136`, `main/process.c:418`) while the
camera can be streaming during QR scans. Design §3.4 resolves this.

---

## 3. Design

### 3.1 Files that change

| File | Change | Required? |
|---|---|---|
| `main/power/tdisplays3pro.inc` | Include `power/i2c.inc`; add SY6970 register defines; PMU init in `power_init()`; real `enable_usb_host()` / `disable_usb_host()`; real `usb_is_powered()`; (optional) real `power_get_vbat()` / `power_get_vusb()` | **Yes** (primary) |
| `main/Kconfig.projbuild` | Give `I2C_SDA` / `I2C_SCL` a `default 5` / `default 6` for `BOARD_TYPE_TTGO_TDISPLAYS3PROCAMERA`, so the hidden symbols resolve to the camera/PMU bus. | **Yes** |
| `main/camera.c` | Board-conditional: for `CONFIG_BOARD_TYPE_TTGO_TDISPLAYS3PROCAMERA`, tell the camera to reuse the PMU bus (`pin_sccb_sda = -1`, `sccb_i2c_port = 0`) instead of creating its own. | **Recommended** (see §3.4) |
| `configs/sdkconfig_display_ttgo_tdisplays3procamera*.defaults` | Only if a config value must change. Prefer Kconfig defaults (hidden-symbol trap, §6). | Likely no |

`main/power.c` routing already points at this file and needs no change.

### 3.2 Why Kconfig (not the board defaults file)

The `I2C_SDA`/`I2C_SCL` menu is `visible if BOARD_TYPE_CUSTOM`
(`main/Kconfig.projbuild:200-239`) and defaults to `-1` for this board
(`:210, :218`; confirmed in `sdkconfig_tdisplays3pro:506-507`). A hand-edit of
the board `.defaults` file for a **hidden** symbol is unreliable — exactly the
trap documented for `CAMERA_XCLK_FREQ` (`configs/sdkconfig_display_ttgo_tdisplays3procamera_camtweaks.defaults:3-4`).
Add the defaults in `Kconfig.projbuild` instead:

```
default 5 if ... || BOARD_TYPE_TTGO_TDISPLAYS3PROCAMERA   # I2C_SDA
default 6 if ... || BOARD_TYPE_TTGO_TDISPLAYS3PROCAMERA   # I2C_SCL
```

Keep `I2C_MASTER_CLK` at the existing 40000 (`:219-222`) — the SY6970 supports
400 kHz and the camera SCCB already runs at 100 kHz on the same wires.

### 3.3 SY6970 init and the OTG sequence

New constants at the top of `main/power/tdisplays3pro.inc`:

```
#define SY6970_ADDR        0x6A
#define SY6970_REG_SYS     0x03   // SYS_CONTROL: bit4 CHG_CONFIG, bit5 OTG_CONFIG
#define SY6970_REG_BOOST   0x0A   // bits7:4 BOOSTV, bits1:0 BOOST_LIM
#define SY6970_REG_STATUS  0x0B   // bits7:5 BUS_STAT, bits4:3 CHG_STAT
#define SY6970_REG_VBAT    0x0E   // 2304 mV + 20 mV/step
#define SY6970_REG_VBUS    0x11   // 2600 mV + 100 mV/step
```

`power_init()` (extend `:33-37`):

1. `ESP_ERROR_CHECK(brightness_init())` (unchanged).
2. `_power_i2c_init()` (`main/power/i2c.inc:60-72`) — creates the bus on
   `I2C_NUM_0` with the new 5/6 defaults.
3. `_power_i2c_attach_device(SY6970_ADDR, &sy6970)` (`:76-88`).
4. **Probe, don't abort:** read an obviously-valid register (e.g. `REG_STATUS`
   or chip-ID via REG14). If it fails, log a warning and leave `sy6970 = NULL`;
   the device must still boot (it may be a Pro variant / bus fault). `usb_is_powered()`
   then falls back to the TinyUSB signal (§3.5) and host mode is simply unavailable.
5. Do **not** enable OTG here. At boot VBUS is usually present (flashing/logging)
   and `enableOTG()` must not run while `isVbusIn()`.

`enable_usb_host()` (replace `:93`):

```
lock i2c_mutex;
read REG03;  set bit5 (OTG_CONFIG);  write REG03;     // RMW
// optional: read/write REG0A to leave BOOSTV at its 5.0 V default and BOOST_LIM
// low (~500 mA is plenty for a thumb drive / reader)
unlock;
```

Called from `usbstorage_impl()` at `usbhmsc.c:104-106`, i.e. after `serial_stop()`
and after the `!usb_is_powered()` assert. At that point USB input is absent, so the
boost is allowed to come up. The USB host driver retries enumeration, so no
explicit settle delay is required (a short `vTaskDelay` is harmless if needed).
Per the datasheet/XPowersLib, enabling OTG automatically disables charging.

`disable_usb_host()` (replace `:92`):

```
lock i2c_mutex;
read REG03;  clear bit5;  write REG03;
unlock;
```

Called from `usbhmsc.c:225-226` on teardown. Clearing OTG makes the chip
re-enable charging (XPowersLib `disableOTG()` comment), which is the power-on
default; we do **not** additionally force `CHG_CONFIG` (see §4, no-battery FAQ).

### 3.4 I2C bus sharing / sequencing with the camera

**Recommended: single shared bus.**

- The PMU owns `i2c_master` port 0 (via `i2c.inc`, unchanged helper).
- For this board only, `main/camera.c` sets
  `.pin_sscb_sda = -1`, `.pin_sscb_scl = -1`, `.sccb_i2c_port = 0`, so
  `camera_probe()` takes the `SCCB_Use_Port(0)` branch
  (`esp_camera.c:175-178`) and **adds the sensor device to the existing PMU
  bus** instead of creating a second controller.
- `SCCB_Use_Port()` does not own the port (`sccb-ng.c:144-157`), and
  `SCCB_Deinit()` returns early when it does not own it
  (`sccb-ng.c:159-181`), so `esp_camera_deinit()`
  (`main/camera.c:334-341`, `esp_camera.c:356`) removes the sensor device but
  leaves the PMU bus alive.
- Because both devices are on the **same** bus instance, the IDF `i2c_master`
  driver's internal per-bus locking serialises PMU reads and camera writes; no
  custom mutex is needed across the two subsystems.

Sequencing guarantees:
- `power_init()` runs at `main/main.c:196`, before the first camera use
  (`main/main.c:256-259`), so the bus always exists when the camera starts.
- The camera is init/stop per use (`main/camera.c:288`, `:336`); PMU transactions
  continue to work between/after those.
- Guard the `camera.c` change with
  `#if defined(CONFIG_BOARD_TYPE_TTGO_TDISPLAYS3PROCAMERA)` so every other board
  (Jade v1/v2 with separate camera pins, M5 CoreS3, Waveshare) keeps its current
  own-bus behaviour.

**Fallback if we want zero `camera.c` changes:** let the PMU create port 0 while
the camera keeps creating port 1 on the same GPIO5/6. This works in isolation but
is two masters on one pair of wires; arbitration/clock-stretch during a
camera-init burst concurrent with a PMU poll is not guaranteed by the IDF driver.
Not recommended for a wallet; only use if the `camera.c` change is unacceptable.

### 3.5 `usb_is_powered()` semantics

Replace `usb_is_powered()` (`:95`) with a PMU-based implementation:

```
if (!sy6970)  return tud_mounted() && !tud_suspended();   // defensive fallback
lock i2c_mutex;
ok = read REG0B -> status;
unlock;
if (!ok)      return false;                                // fail-open toward host mode
bus = (status >> 5) & 0x7;
return bus != 0 /*NO_INPUT*/ && bus != 7 /*OTG*/;
```

Rationale:
- `BUS_STAT` in {SDP, CDP, DCP, HVDCP, adapter, non-standard} means **VBUS is
  present and the board is a sink/charging** → `true` (matches the "USB powered"
  meaning used by `usbmode.c:283`, `usbhmsc.c:256`, idle/disconnect logic).
- `NO_INPUT` (0) → running on battery → `false`.
- `OTG` (7) → the board is sourcing VBUS, not a sink → `false`. This is essential:
  once the host stack enables OTG, `usb_is_powered()` must read `false` so the
  `usbhmsc.c:256` assertion and the idle/disconnect checks stay consistent.
- On I2C failure, returning `false` lets the storage flow proceed; the alternative
  (`true`) would re-hang the `while` loop. Log the failure.
- The TinyUSB fallback (`tdisplays3.inc:179-191` pattern) keeps the device usable
  if the PMU is absent/unreadable; during host mode TinyUSB is uninstalled, so the
  fallback also reads `false`, which is correct.

No `power_get_vusb()` dependency: the bus status is a single 1-byte read, so the
feature does not require real VBUS/battery voltages.

### 3.6 Do `power_get_vusb()` / `power_get_vbat()` need real values?

**Not required for this feature.** `usb_is_powered()` uses REG0B only.

Recommended as a low-risk adjacent improvement (optional, can be a follow-up):
- `power_get_vbat()` → read REG0E: `2304 + 20 * raw` mV.
- `power_get_vusb()` → if `BUS_STAT != 0`, read REG11: `2600 + 100 * raw` mV.
- Feeding real VBAT lets `power_get_battery_status()` (`:84`) and the GUI battery
  icon (`main/gui.c:2329-2339`) work; `power_get_battery_charging()` (`:85`) could
  use REG0B bits4:3.

Because these touch the same file and the same bus, doing them now is cheap; they
are, however, **not** needed to make storage work and should not block it.

---

## 4. Risks and gotchas

1. **Role switch loses the CDC/console port.** `serial_stop()`
   (`main/serial.c:306-334`) uninstalls TinyUSB before the host stack starts, and
   TinyUSB is only reinstalled by `serial_start()` after teardown
   (`usbmode.c:367`, `:664`, `usbhmsc.c:225-226`). While hosting, the USB-C port
   is a host, so the PC sees no CDC (this board has a single OTG-capable Type-C;
   LilyGO wiki "USB 1 × Type-C (OTG capable)"). If the firmware wedges in host
   mode, recovery requires manual download mode — **hold BOOT, tap RST, release
   BOOT** (LilyGO README FAQ), then reflash.
2. **Must run on battery while hosting.** OTG can only be enabled with no VBUS
   input (XPowersLib `enableOTG()` refuses when `isVbusIn()`); the same cable/port
   cannot be both a PC VBUS source and the host. The `while (usb_is_powered())`
   gate (`usbmode.c:283-288`) enforces disconnecting USB power first.
3. **`usbhmsc.c:256` assertion.** `JADE_ASSERT(!usb_is_powered())` must be true at
   host start. A stale OTG bit retained by the PMU across an ESP reset, or an I2C
   mis-read, could make `usb_is_powered()` report `true` and abort. Mitigations:
   clear OTG early in `power_init()` (`REG03` bit5 = 0) so boot always starts from
   a known sink/charging state; keep the I2C read robust and logged.
4. **Unmount / serial ordering.** The existing sequence is
   `serial_stop()` → `usbstorage_start()` (host up) → action → `usbstorage_stop()`
   → `serial_start()` (`usbmode.c:290-300, 362-368`; async OTA path at `:655-665`).
   Any new `usb_is_powered()` must **not** depend on TinyUSB state (it doesn't —
   PMU only), otherwise calls from `gui.c`/`idletimer.c`/`process.c` during the
   serial-stopped window would misbehave. Also note the OTA worker unmounts and
   restarts serial after a reboot is already pending.
5. **Two-master hazard if §3.4 fallback is chosen** (PMU port 0 + camera port 1 on
   GPIO5/6). Prefer the single-bus design.
6. **Regression risk from the now-real idle/disconnect behaviour.** On battery,
   idle timeout will now `POWER_OFF` (`idletimer.c:136,160-164`) instead of never
   sleeping. If a drive is mounted when the device deep-sleeps
   (`power_shutdown()` is `esp_deep_sleep_start()`, `tdisplays3pro.inc:39-44`),
   the filesystem is not cleanly unmounted. This is a general Jade behaviour, but
   it is newly reachable here — include it in the on-device test.
7. **No-battery power stability.** LilyGO FAQ: with no battery and USB attached,
   SY6970 `CHG_CONFIG` should be off to avoid brown-out/reboot. Enabling OTG
   auto-disables charging (datasheet/XPowersLib), so hosting should be stable, but
   we deliberately do **not** change the power-on charging default. Flag for
   on-device check; do not silently "fix" as part of this feature.
8. **OTG enable pin caveat.** Datasheet says boost needs `REG03[5]=1` **and the
   OTG pin high**; LilyGO exposes no such GPIO and their example only writes the
   register. Verify on hardware that a drive actually powers (measure 5 V or watch
   enumeration); if not, find the strap/GPIO on the schematic. (§7 open question.)
9. **`power_shutdown()` uses deep-sleep**, not PMU power-off. Unrelated to USB but
   share the battery/idle test.
10. **Format gate.** `format.sh:14` runs `clang-format-19` over
    `main/**/*.{c,h,inc}`, so the edited `.inc` (and `camera.c`) must be formatted.

---

## 5. Verification (on-device; there is no CI hardware)

### 5.1 Build (fork command)

```sh
source /Volumes/Crucial2T/Mac/leo_temp/lilygo/esp/esp-idf/export.sh
idf.py -B build_s3pro -DSDKCONFIG=sdkconfig_tdisplays3pro \
  "-DSDKCONFIG_DEFAULTS=configs/sdkconfig_display_ttgo_tdisplays3procamera.defaults;configs/sdkconfig_display_ttgo_tdisplays3_usbfix.defaults;configs/sdkconfig_display_ttgo_tdisplays3procamera_camtweaks.defaults" \
  build
```

After editing `Kconfig.projbuild`, delete `sdkconfig_tdisplays3pro` (and
`build_s3pro/CMakeCache.txt` if needed) and confirm the new defaults landed:

```sh
rg 'CONFIG_I2C_SDA|CONFIG_I2C_SCL' sdkconfig_tdisplays3pro   # expect 5 and 6
```

### 5.2 Flash

Download mode: **hold BOOT, tap RST, release BOOT**, then:

```sh
idf.py -B build_s3pro -p /dev/cu.usbmodem14101 flash
```

Release BOOT, tap RST; app enumerates as `/dev/cu.usbmodem1234561`.

### 5.3 Baseline (no feature)

- [ ] RPC PING and `get_version_info` return `BOARD_TYPE: TTGO_TDISPLAYS3PROCAMERA`
      (jadepy snippet in `.opencode/skills/jade-diy-fw/SKILL.md` §4).
- [ ] On USB power, the status-bar indicator shows `C`; unplug and run on battery,
      it shows `D` (proves the new `usb_is_powered()`).
- [ ] Camera QR scan still works (bus sharing / no SCCB regression).
- [ ] Backlight still dims correctly (GPIO48 LEDC unchanged).

### 5.4 Prepare a FAT32 drive

- Format a USB stick / SD reader as **FAT32**, with:
  - a compressed firmware pair `<name>_<fwsize>_*_fw.bin` and `<name>_<fwsize>_*_fw.bin.hash`
    (e.g. produced by `generate_fw_upload.sh`),
  - a `test.psbt` (binary or base64),
  - optionally subfolders (the action lists only regular files).
- Confirm FAT32 only: exFAT/NTFS should fail with "only FAT32 is supported"
  (`usbmode.c:316-319`).

### 5.5 Exercise each action (run on battery)

From **Options → USB Storage** (unlocked wallet for Sign/Export):

1. **Export Xpub** — with the drive attached and no USB power, the prompt appears;
   after export `jade-xpub.txt` exists on the drive and the screen shows
   "xpub saved to jade-xpub.txt" (`usbmode.c:1041-1050`).
2. **Sign PSBT** — select `test.psbt`; on success `test_signed.psbt` is written
   next to it (`usbmode.c:846-883`); verify the signature with `bitcoin-cli`/
   `jadepy` off-device.
3. **Firmware Upgrade** — select the `*_fw.bin`; signing/confirmation flow runs,
   device reboots into the new firmware; `get_version_info` changes.

Pass = all three complete with no watchdog reset and the screen stays usable.

### 5.6 Negative / edge cases

- [ ] Start an action with **no drive**: >5 s prompt "connect a storage device"
      appears (`usbmode.c:331-341`), then plugging a drive mounts and continues.
- [ ] Unplug the drive mid-action / mid-copy: error dialog, no panic/reboot loop
      (`usbmode.c:316-319`, `usbhmsc.c:151-157`).
- [ ] After any action, reconnect to the PC: the **CDC port returns** (serial
      restarted) and RPC works again.
- [ ] Idle on battery past the timeout: device powers off (new, expected).
- [ ] Optional: battery present vs absent — confirm no brown-out on OTG hot-plug.

> Logging: the board build sets `CONFIG_LOG_DEFAULT_LEVEL_NONE`
> (`configs/...procamera.defaults:57`), so there is no boot log. Use a temporary
> debug build or on-screen/RPC evidence for the checks above.

---

## 6. CI impact

No new pipeline job is needed; the feature is hardware-dependent and cannot run
on the headless builder (see `docs/plans/ci-and-test-strategy.md` §3).

| Job | File | Expected impact |
|---|---|---|
| `build_diy_display_ttgo_tdisplays3procamera` | `gitlab/diy_fw.yml:42` (template `:7`) | **Must stay green** — compiles the edited `tdisplays3pro.inc`, `camera.c`, `Kconfig.projbuild` |
| `test_format` | `gitlab/test.yml:14` → `format.sh:14` | **Must stay green** — `.inc`, `.c` are clang-formatted; `idf.py reconfigure` + `git diff --exit-code` |
| `test_configs` | `gitlab/test.yml:24` → `tools/check_default_configs.sh` | **Expected unaffected** — the script only regenerates `configs/**/*jade*.defaults` (`check_default_configs.sh:16,27`), i.e. NOT the ttgo display configs. Preferring Kconfig for the hidden `I2C_SDA/SCL` means no board `.defaults` edit at all. |
| `test_libjade*` | `gitlab/test_libjade.yml` | Unaffected (no host-side code) |

Toolchain divergence to confirm: local DIY builds use **ESP-IDF v5.1.2**; CI uses
the pinned `blockstream/jade_builder` image (`.gitlab-ci.yml:19`) whose Dockerfile
targets a newer IDF (`gitlab/docker.yml:24` → `v5.5.4`). The `i2c_master` API used
here exists in both, and the camera's `SCCB_HARDWARE_I2C_PORT*` / `sccb_i2c_port`
options exist in the pinned `esp32-camera 2.0.15`. Confirm the pinned image digest
actually contains the expected IDF version before trusting a green local vs CI
difference (also tracked in `ci-and-test-strategy.md` §4).

---

## 7. Open questions / decisions

1. **Single-bus vs two-master** (§3.4): proceed with the recommended single-bus
   (one small board-conditional edit in `camera.c`), or stay strictly inside
   `tdisplays3pro.inc` + Kconfig and accept the two-master risk? *Recommend
   single-bus.*
2. **OTG enable pin**: confirm on the schematic/hardware that `REG03[5]=1` alone
   powers the port (datasheet mentions an OTG pin). If a GPIO is involved, wire it
   into `enable_usb_host()`/`disable_usb_host()`.
3. **Vbat/Vusb**: implement now (same file, feeds UI) or defer as a follow-up?
   *Not required for this feature.*
4. **Charging default / no-battery stability** (§4.7): leave the power-on charging
   default untouched (this plan), or add a no-battery `CHG_CONFIG` disable later?
5. **Clear stale OTG at boot** (§4.3): include the defensive `REG03` bit5 clear in
   `power_init()`? *Recommend yes.*

---

## 8. Task breakdown and effort estimate

Ordered, each a small independently reviewable step. Effort is rough
(half-day granularity, one firmware engineer, excluding soak testing).

| # | Task | Est. |
|---|---|---|
| 1 | Kconfig: add `I2C_SDA=5` / `I2C_SCL=6` defaults for the Pro board; delete generated sdkconfig; rebuild; confirm values. | 0.25 d |
| 2 | `tdisplays3pro.inc`: include `i2c.inc`, add SY6970 defines, init bus + attach + probe in `power_init()` (log-and-continue on failure), clear stale OTG. | 0.5 d |
| 3 | Implement `enable_usb_host()` / `disable_usb_host()` (REG03 bit5 RMW under the mutex). | 0.25 d |
| 4 | Implement `usb_is_powered()` from REG0B (with TinyUSB fallback + error handling). | 0.25 d |
| 5 | `camera.c`: board-conditional SCCB reuse of the PMU bus (single-bus design); verify QR scan and passphrase scan. | 0.5 d |
| 6 | Build + clang-format; fix any Kconfig/format gate fallout. | 0.25 d |
| 7 | On-device: baseline indicators, FAT32 mount, Export Xpub, Sign PSBT, FW upgrade, negatives, idle power-off. | 1.0–1.5 d |
| 8 | (Optional) real `power_get_vbat()` / `power_get_vusb()` + GUI battery check. | 0.25 d |
| 9 | Update this doc with results; confirm CI (`build_diy_*`, `test_format`, `test_configs`). | 0.25 d |

Rough total: **~3.5–4 focused days**, of which ~1–1.5 days is on-device
verification that cannot be parallelised.
```
