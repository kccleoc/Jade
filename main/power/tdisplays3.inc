// T-Display S3 implementation without PMIC (ADC-based)
//

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_idf_version.h>
#include <esp_sleep.h>

#include "tusb.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
#define BATTERY_ADC_ATTEN ADC_ATTEN_DB_12
#else
#define BATTERY_ADC_ATTEN ADC_ATTEN_DB_11
#endif
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_3
#define BATTERY_EMA_ALPHA 0.3f // estimated moving average smoothing factor

#define LCD_BL_GPIO CONFIG_LCD_BACKLIGHT_PIN_NUM
#define LCD_BL_LEDC_TIMER LEDC_TIMER_0
#define LCD_BL_LEDC_MODE LEDC_LOW_SPEED_MODE
#define LCD_BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define LCD_BL_LEDC_DUTY_RES LEDC_TIMER_10_BIT
#define LCD_BL_LEDC_DUTY (1024)
#define LCD_BL_LEDC_FREQUENCY (10000)
#define BATTERY_EMA_ALPHA 0.3f // estimated moving average smoothing factor

static adc_oneshot_unit_handle_t adc1_handle = NULL;
static adc_cali_handle_t adc1_cali_chan0_handle = NULL;

static float ema_voltage = 0.0f;

static esp_err_t brightness_init(void)
{
    gpio_reset_pin(LCD_BL_GPIO);
    gpio_set_direction(LCD_BL_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_BL_GPIO, 1);
    ledc_timer_config_t ledc_timer = { .speed_mode = LCD_BL_LEDC_MODE,
        .timer_num = LCD_BL_LEDC_TIMER,
        .duty_resolution = LCD_BL_LEDC_DUTY_RES,
        .freq_hz = LCD_BL_LEDC_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));
    ledc_channel_config_t ledc_channel = { .speed_mode = LCD_BL_LEDC_MODE,
        .channel = LCD_BL_LEDC_CHANNEL,
        .timer_sel = LCD_BL_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LCD_BL_GPIO,
        .duty = 0, // Set duty to 0%
        .hpoint = 0 };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
    return ESP_OK;
}

esp_err_t power_init(void)
{
    // Initialise backlight brightness
    ESP_ERROR_CHECK(brightness_init());
    // Initialise the ADC to measure battery level
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));
    JADE_ASSERT(adc1_handle);
    // ADC Config
    adc_oneshot_chan_cfg_t config = {
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, BATTERY_ADC_CHANNEL, &config));
    // Use voltage calibration
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &adc1_cali_chan0_handle) != ESP_OK) {
        JADE_LOGW("ADC calibration not available, continuing without it");
        adc1_cali_chan0_handle = NULL;
    }
    return ESP_OK;
}

esp_err_t power_shutdown(void)
{
    esp_deep_sleep_start();
    return ESP_OK;
}
esp_err_t power_screen_on(void) { return ESP_OK; }
esp_err_t power_screen_off(void) { return ESP_OK; }
static void backlight_apply(const uint32_t duty)
{
    // (Re-)attach the pin to LEDC and apply the duty in one step - this also
    // reclaims the pin if display init has reconfigured it as a plain gpio
    ledc_channel_config_t ledc_channel = { .speed_mode = LCD_BL_LEDC_MODE,
        .channel = LCD_BL_LEDC_CHANNEL,
        .timer_sel = LCD_BL_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LCD_BL_GPIO,
        .duty = duty,
        .hpoint = 0 };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

esp_err_t power_backlight_on(uint8_t brightness)
{
    if (brightness < BACKLIGHT_MIN) {
        brightness = BACKLIGHT_MIN;
    } else if (brightness > BACKLIGHT_MAX) {
        brightness = BACKLIGHT_MAX;
    }
    // Map levels 1-5 onto 25%-100% duty cycle
    const uint32_t duty = 256 + ((brightness - 1) * (LCD_BL_LEDC_DUTY - 1 - 256)) / 4;
    backlight_apply(duty);
    return ESP_OK;
}
esp_err_t power_backlight_off(void)
{
    backlight_apply(0);
    return ESP_OK;
}
esp_err_t power_camera_on(void) { return ESP_OK; }
esp_err_t power_camera_off(void) { return ESP_OK; }

uint16_t power_get_vbat(void)
{
    JADE_ASSERT(adc1_handle);
    int raw_adc, voltage;
    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, BATTERY_ADC_CHANNEL, &raw_adc));
    if (adc1_cali_chan0_handle) {
        ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_chan0_handle, raw_adc, &voltage));
    } else {
        voltage = (raw_adc * 3300) / 4095;
    }
    // Apply Estimated Moving Average (EMA)
    const float ema_v = (ema_voltage > 0) ? ema_voltage : (float)voltage;
    ema_voltage = (BATTERY_EMA_ALPHA * voltage) + ((1.0f - BATTERY_EMA_ALPHA) * ema_v);
    // T-Display S3 voltage divider is 2 (100k/100k)
    return (uint16_t)(ema_voltage * 2);
}

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

bool power_get_battery_charging(void)
{
    // The charging IC STAT pin is not connected to GPIO.
    // Return USB status as a proxy for charging (Dummy status)
    return usb_is_powered();
}

uint16_t power_get_ibat_charge(void) { return 0; }
uint16_t power_get_ibat_discharge(void) { return 0; }
uint16_t power_get_vusb(void) { return 0; }
uint16_t power_get_iusb(void) { return 0; }
uint16_t power_get_temp(void) { return 0; }

void disable_usb_host(void) {}
void enable_usb_host(void) {}

bool usb_is_powered(void)
{
    // Check if USB is mounted and not suspended (active connection)
    if (tud_mounted() && !tud_suspended()) {
        return true;
    }
    // Fallback: If voltage is near zero (<1V) but chip is on,
    // we must be running on USB power without a battery.
    if (power_get_vbat() < 1000) {
        return true;
    }
    return false;
}
