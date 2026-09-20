// T-Display S3 Pro implementation - PWM backlight on the dedicated backlight pin
// (GPIO48), plus the on-board SY6970 PMU (I2C) used for USB OTG host power,
// VBUS presence detection and battery voltage/level.  Current/temperature
// values remain stubbed out.
//
// The SY6970 and the camera SCCB share the same two I2C wires (GPIO 5/6), so the
// PMU creates the single master bus that the camera then reuses (see camera.c).
//
// Two I2C driver generations must be supported, matching the esp32-camera
// component's own choice (sccb.c vs sccb-ng.c):
//   - IDF < 5.4  : camera uses the legacy i2c driver, so the PMU does too
//                  (this is what the local DIY toolchain, v5.1.2, builds).
//   - IDF >= 5.4 : camera uses the new i2c_master driver, so the PMU does too
//                  (this is what the CI builder image, v5.5.4, builds).
//

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_idf_version.h>
#include <esp_sleep.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "tusb.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
#include <driver/i2c_master.h>
static i2c_master_bus_handle_t pmu_bus = NULL;
static i2c_master_dev_handle_t pmu_dev = NULL;
#else
#include <driver/i2c.h>
#define PMU_I2C_PORT I2C_NUM_0
#endif

#define LCD_BL_GPIO CONFIG_DISPLAY_PIN_BL
#define LCD_BL_LEDC_TIMER LEDC_TIMER_0
#define LCD_BL_LEDC_MODE LEDC_LOW_SPEED_MODE
#define LCD_BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define LCD_BL_LEDC_DUTY_RES LEDC_TIMER_10_BIT
#define LCD_BL_LEDC_DUTY 1023
#define LCD_BL_LEDC_FREQUENCY 10000

// SY6970 PMU register map (7-bit I2C address 0x6a).
#define SY6970_ADDR 0x6A
#define SY6970_REG_SYS 0x03 // SYS_CONTROL: bit4 CHG_CONFIG, bit5 OTG_CONFIG
#define SY6970_REG_STATUS 0x0B // bits7:5 BUS_STAT, bits4:3 CHG_STAT
#define SY6970_OTG_CONFIG_MASK (0x1 << 5)
#define SY6970_BUS_STAT_SHIFT 5
#define SY6970_BUS_STAT_MASK 0x7
#define SY6970_BUS_STAT_NO_INPUT 0x0
#define SY6970_BUS_STAT_OTG 0x7

// ADC control: enable continuous conversion so the VBAT register stays fresh.
#define SY6970_REG_ADC_CTRL 0x02
#define SY6970_ADC_CONV_START (0x1 << 7) // bit7: start a conversion
#define SY6970_ADC_CONV_RATE (0x1 << 6) // bit6: 1 = continuous, 0 = one-shot

// Battery voltage ADC (read-only): bit7 THERM_STAT (ignored), bits6:0 BATV.
// VBAT_mV = 2304 + BATV * 20, as per the SY6970/BQ25890 datasheet.
#define SY6970_REG_VBAT 0x0E
#define SY6970_VBAT_MASK 0x7F
#define SY6970_VBAT_BASE_MV 2304
#define SY6970_VBAT_STEP_MV 20

// Charging status bits (REG_STATUS bits4:3).
#define SY6970_CHG_STAT_SHIFT 3
#define SY6970_CHG_STAT_MASK 0x3
#define SY6970_CHG_STAT_PRECHARGE 0x1
#define SY6970_CHG_STAT_FAST 0x2

// PMU I2C timeout.
#define PMU_I2C_TIMEOUT_MS 1000

// Shorter timeout for the ~1/sec battery polls, so a busy shared bus (camera)
// can never stall the GUI/entropy callers for a full second.
#define VBAT_I2C_TIMEOUT_MS 100

// Minimum plausible pack voltage - readings below this are treated as invalid
// or 'no battery' and the last known value is retained.
#define VBAT_MIN_VALID_MV 2500

static SemaphoreHandle_t pmu_mutex = NULL;
static bool pmu_bus_ready = false;
static bool pmu_ok = false;

// Last known-good pack voltage (mV), retained across transient bus errors or
// implausible readings so the battery icon does not flicker to empty.
static uint16_t last_vbat_mv = 0;

// Brightness levels 1-5 mapped to these duty percentages.  Level 3 is the
// default and corresponds to 50% duty - half of the previous always-on level.
static const uint8_t BRIGHTNESS_PERCENT[BACKLIGHT_MAX] = { 25, 40, 50, 75, 100 };

static esp_err_t brightness_init(void)
{
    ledc_timer_config_t ledc_timer = { .speed_mode = LCD_BL_LEDC_MODE,
        .timer_num = LCD_BL_LEDC_TIMER,
        .duty_resolution = LCD_BL_LEDC_DUTY_RES,
        .freq_hz = LCD_BL_LEDC_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));
    return ESP_OK;
}

// Create the shared master bus on I2C_NUM_0 (GPIO 5/6) using the I2C driver
// generation that matches the camera component for this IDF version.
static esp_err_t pmu_bus_init(void)
{
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
    const i2c_master_bus_config_t bus_cfg = { .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_I2C_SDA,
        .scl_io_num = CONFIG_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &pmu_bus);
    if (err != ESP_OK) {
        return err;
    }
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = SY6970_ADDR, .scl_speed_hz = CONFIG_I2C_MASTER_CLK
    };
    return i2c_master_bus_add_device(pmu_bus, &dev_cfg, &pmu_dev);
#else
    const i2c_config_t cfg = { .mode = I2C_MODE_MASTER,
        .sda_io_num = CONFIG_I2C_SDA,
        .scl_io_num = CONFIG_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = CONFIG_I2C_MASTER_CLK };
    esp_err_t err = i2c_param_config(PMU_I2C_PORT, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_driver_install(PMU_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
#endif
}

static esp_err_t pmu_read_reg_to(const uint8_t reg, uint8_t* val, const uint32_t timeout_ms)
{
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
    return i2c_master_transmit_receive(pmu_dev, &reg, 1, val, 1, timeout_ms);
#else
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (SY6970_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (SY6970_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, val, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    const TickType_t ticks = (timeout_ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS;
    const esp_err_t err = i2c_master_cmd_begin(PMU_I2C_PORT, cmd, ticks);
    i2c_cmd_link_delete(cmd);
    return err;
#endif
}

static esp_err_t pmu_read_reg(const uint8_t reg, uint8_t* val) { return pmu_read_reg_to(reg, val, PMU_I2C_TIMEOUT_MS); }

static esp_err_t pmu_write_reg(const uint8_t reg, const uint8_t val)
{
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(pmu_dev, buf, sizeof(buf), PMU_I2C_TIMEOUT_MS);
#else
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (SY6970_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    const esp_err_t err = i2c_master_cmd_begin(PMU_I2C_PORT, cmd, PMU_I2C_TIMEOUT_MS / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    return err;
#endif
}

esp_err_t power_init(void)
{
    ESP_ERROR_CHECK(brightness_init());

    pmu_mutex = xSemaphoreCreateMutex();
    if (!pmu_mutex) {
        JADE_LOGE("Failed to create PMU mutex");
        return ESP_OK;
    }

    // Bring up the shared I2C bus and attach the SY6970 PMU.  Failures are
    // logged and tolerated: the device must still boot (e.g. a Pro variant
    // without the PMU, or a bus fault), just without USB host support.
    const esp_err_t bus_err = pmu_bus_init();
    if (bus_err != ESP_OK) {
        JADE_LOGE("Failed to init PMU i2c bus: %u", bus_err);
        return ESP_OK;
    }
    pmu_bus_ready = true;

    // Probe with a status register read; on failure leave the PMU unused.
    uint8_t status = 0;
    if (pmu_read_reg(SY6970_REG_STATUS, &status) != ESP_OK) {
        JADE_LOGW("SY6970 PMU not responding, disabling USB host support");
        return ESP_OK;
    }
    pmu_ok = true;
    JADE_LOGI("SY6970 PMU detected (status: 0x%02x)", status);

    // Enable the PMU ADC in continuous-conversion mode so the battery voltage
    // register (REG 0x0E) is kept up to date (~1Hz) for the status bar.
    uint8_t adc_ctrl = 0;
    JADE_SEMAPHORE_TAKE(pmu_mutex);
    if (pmu_read_reg(SY6970_REG_ADC_CTRL, &adc_ctrl) == ESP_OK) {
        const esp_err_t err = pmu_write_reg(SY6970_REG_ADC_CTRL, adc_ctrl | SY6970_ADC_CONV_RATE | SY6970_ADC_CONV_START);
        if (err != ESP_OK) {
            JADE_LOGW("Failed to enable SY6970 ADC: %u", err);
        }
    }
    JADE_SEMAPHORE_GIVE(pmu_mutex);

    // Boot in a known sink/charging state: clear any OTG-enable left set across
    // a reset, so we never start up sourcing VBUS to a device that is not there.
    disable_usb_host();

    return ESP_OK;
}

// Whether the shared PMU I2C bus exists - the camera uses this to decide whether
// to reuse it (see camera.c) or fall back to its own bus.
bool power_pmu_i2c_ready(void) { return pmu_bus_ready; }

esp_err_t power_shutdown(void)
{
    // If we don't have a PMU, use esp_deep_sleep()
    esp_deep_sleep_start();
    return ESP_OK;
}

esp_err_t power_screen_on(void) { return ESP_OK; }

esp_err_t power_backlight_on(uint8_t brightness)
{
    if (brightness < BACKLIGHT_MIN) {
        brightness = BACKLIGHT_MIN;
    } else if (brightness > BACKLIGHT_MAX) {
        brightness = BACKLIGHT_MAX;
    }

    const uint32_t duty = (LCD_BL_LEDC_DUTY * BRIGHTNESS_PERCENT[brightness - 1]) / 100;

    // (Re-)attach the pin to LEDC and apply the duty - display init configures
    // the backlight pin as a plain gpio after power_init.
    ledc_channel_config_t ledc_channel = { .speed_mode = LCD_BL_LEDC_MODE,
        .channel = LCD_BL_LEDC_CHANNEL,
        .timer_sel = LCD_BL_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LCD_BL_GPIO,
        .duty = duty,
        .hpoint = 0 };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
    return ESP_OK;
}

esp_err_t power_backlight_off(void)
{
    ledc_stop(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL, 0);
    gpio_reset_pin(LCD_BL_GPIO);
    gpio_set_direction(LCD_BL_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_BL_GPIO, 0);
    return ESP_OK;
}

esp_err_t power_camera_on(void) { return ESP_OK; }
esp_err_t power_camera_off(void) { return ESP_OK; }

// Read the SY6970 battery-voltage ADC (mV), or 0 if no battery is attached.
// The last known-good value is returned across transient bus errors, and kept
// for implausible readings, so the UI never flickers to empty on a glitch.
uint16_t power_get_vbat(void)
{
    if (!pmu_ok || !pmu_mutex) {
        return 0;
    }

    uint8_t reg = 0;
    JADE_SEMAPHORE_TAKE(pmu_mutex);
    const esp_err_t err = pmu_read_reg_to(SY6970_REG_VBAT, &reg, VBAT_I2C_TIMEOUT_MS);
    JADE_SEMAPHORE_GIVE(pmu_mutex);
    if (err != ESP_OK) {
        return last_vbat_mv;
    }

    const uint8_t raw = reg & SY6970_VBAT_MASK;
    if (raw == 0) {
        // The SY6970 reports 0 when no battery is connected.
        return 0;
    }

    const uint16_t mv = SY6970_VBAT_BASE_MV + (raw * SY6970_VBAT_STEP_MV);
    if (mv < VBAT_MIN_VALID_MV) {
        return last_vbat_mv;
    }

    last_vbat_mv = mv;
    return last_vbat_mv;
}

// Map the pack voltage to the 0-5 battery level the UI expects (0 = empty,
// 5 = full), matching the other ADC-based boards.
uint8_t power_get_battery_status(void)
{
    const uint16_t vbat = power_get_vbat();

    if (vbat > 4000) {
        return 5;
    } else if (vbat > 3800) {
        return 4;
    } else if (vbat > 3600) {
        return 3;
    } else if (vbat > 3400) {
        return 2;
    } else if (vbat > 3200) {
        return 1;
    }
    return 0;
}

// True while the SY6970 is actively charging (pre-charge or fast-charge).
bool power_get_battery_charging(void)
{
    if (!pmu_ok || !pmu_mutex) {
        return false;
    }

    uint8_t status = 0;
    JADE_SEMAPHORE_TAKE(pmu_mutex);
    const esp_err_t err = pmu_read_reg_to(SY6970_REG_STATUS, &status, VBAT_I2C_TIMEOUT_MS);
    JADE_SEMAPHORE_GIVE(pmu_mutex);
    if (err != ESP_OK) {
        return false;
    }

    const uint8_t chg_stat = (status >> SY6970_CHG_STAT_SHIFT) & SY6970_CHG_STAT_MASK;
    return chg_stat == SY6970_CHG_STAT_PRECHARGE || chg_stat == SY6970_CHG_STAT_FAST;
}
uint16_t power_get_ibat_charge(void) { return 0; }
uint16_t power_get_ibat_discharge(void) { return 0; }
uint16_t power_get_vusb(void) { return 0; }
uint16_t power_get_iusb(void) { return 0; }
uint16_t power_get_temp(void) { return 0; }

void disable_usb_host(void)
{
    if (!pmu_ok) {
        return;
    }

    uint8_t reg03 = 0;
    JADE_SEMAPHORE_TAKE(pmu_mutex);
    if (pmu_read_reg(SY6970_REG_SYS, &reg03) == ESP_OK) {
        const esp_err_t err = pmu_write_reg(SY6970_REG_SYS, reg03 & ~SY6970_OTG_CONFIG_MASK);
        if (err != ESP_OK) {
            JADE_LOGW("Failed to disable SY6970 OTG: %u", err);
        }
    }
    JADE_SEMAPHORE_GIVE(pmu_mutex);
}

void enable_usb_host(void)
{
    if (!pmu_ok) {
        return;
    }

    uint8_t reg03 = 0;
    JADE_SEMAPHORE_TAKE(pmu_mutex);
    if (pmu_read_reg(SY6970_REG_SYS, &reg03) == ESP_OK) {
        const esp_err_t err = pmu_write_reg(SY6970_REG_SYS, reg03 | SY6970_OTG_CONFIG_MASK);
        if (err != ESP_OK) {
            JADE_LOGW("Failed to enable SY6970 OTG: %u", err);
        }
    }
    JADE_SEMAPHORE_GIVE(pmu_mutex);
}

bool usb_is_powered(void)
{
    if (!pmu_ok) {
        // Defensive fallback for a missing/unreadable PMU: only trust an
        // actively mounted USB connection (matches the non-PMU T-Display S3).
        return tud_mounted() && !tud_suspended();
    }

    uint8_t status = 0;
    JADE_SEMAPHORE_TAKE(pmu_mutex);
    const esp_err_t read_err = pmu_read_reg(SY6970_REG_STATUS, &status);
    JADE_SEMAPHORE_GIVE(pmu_mutex);
    if (read_err != ESP_OK) {
        JADE_LOGW("Failed to read SY6970 bus status: %u", read_err);
        // Fall back to the USB peripheral state: keeps idle-timeout and disconnect
        // logic sane if the PMU read glitches, and reads false during host mode
        // (TinyUSB is uninstalled then) so the storage gate still opens.
        return tud_mounted() && !tud_suspended();
    }

    const uint8_t bus_stat = (status >> SY6970_BUS_STAT_SHIFT) & SY6970_BUS_STAT_MASK;
    // Any input other than 'none' means VBUS is present and we are a sink,
    // except 'OTG' where we are sourcing VBUS and therefore not powered by USB.
    return bus_stat != SY6970_BUS_STAT_NO_INPUT && bus_stat != SY6970_BUS_STAT_OTG;
}
