// T-Display S3 Pro implementation - PWM backlight on the dedicated backlight pin
// (GPIO48).  The board's SY6970 PMIC is not supported, so battery values are
// stubbed out (as with the minimal implementation).
//

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_sleep.h>

#define LCD_BL_GPIO CONFIG_DISPLAY_PIN_BL
#define LCD_BL_LEDC_TIMER LEDC_TIMER_0
#define LCD_BL_LEDC_MODE LEDC_LOW_SPEED_MODE
#define LCD_BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define LCD_BL_LEDC_DUTY_RES LEDC_TIMER_10_BIT
#define LCD_BL_LEDC_DUTY 1023
#define LCD_BL_LEDC_FREQUENCY 10000

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

esp_err_t power_init(void)
{
    ESP_ERROR_CHECK(brightness_init());
    return ESP_OK;
}

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

uint16_t power_get_vbat(void) { return 0; }
uint8_t power_get_battery_status(void) { return 0; }
bool power_get_battery_charging(void) { return false; }
uint16_t power_get_ibat_charge(void) { return 0; }
uint16_t power_get_ibat_discharge(void) { return 0; }
uint16_t power_get_vusb(void) { return 0; }
uint16_t power_get_iusb(void) { return 0; }
uint16_t power_get_temp(void) { return 0; }

void disable_usb_host(void) {}
void enable_usb_host(void) {}

bool usb_is_powered(void) { return true; }
