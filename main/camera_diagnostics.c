#include "esp32_s3_szp.h"

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Failure-only diagnostics. Never run concurrently with esp32-camera/web streaming.
 * Use the same legacy I2C API as the camera component; do not mix driver APIs. */
#define DIAG_I2C_PORT I2C_NUM_0
#define GC2145_ADDRESS 0x3c
#define DIAG_TIMEOUT pdMS_TO_TICKS(50)

static const char *TAG = "camera_diag";

static esp_err_t probe_address(uint8_t address)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = i2c_master_start(cmd);
    if (err == ESP_OK) {
        err = i2c_master_write_byte(cmd, (address << 1) | I2C_MASTER_WRITE, true);
    }
    if (err == ESP_OK) {
        err = i2c_master_stop(cmd);
    }
    if (err == ESP_OK) {
        err = i2c_master_cmd_begin(DIAG_I2C_PORT, cmd, DIAG_TIMEOUT);
    }
    i2c_cmd_link_delete(cmd);
    return err;
}

static esp_err_t read_id_register(uint8_t reg, uint8_t *value)
{
    /* Match the camera driver's SCCB read: STOP between register selection
     * and data read. Only select/read ID registers; no sensor configuration writes. */
    esp_err_t err = i2c_master_write_to_device(DIAG_I2C_PORT, GC2145_ADDRESS,
                                              &reg, 1, DIAG_TIMEOUT);
    if (err == ESP_OK) {
        err = i2c_master_read_from_device(DIAG_I2C_PORT, GC2145_ADDRESS,
                                         value, 1, DIAG_TIMEOUT);
    }
    return err;
}

void bsp_camera_probe_diagnostics(void)
{
    bool timer_ready = false;
    bool channel_ready = false;
    bool bus_ready = false;
    esp_err_t err;

    ESP_LOGW(TAG, "Probe diagnostics v1: original camera init failed; camera output is not started");
    ESP_LOGW(TAG, "RESETB/PWDN voltages are not measured by this diagnostic; check them externally");

    /* Explicitly route a temporary LEDC clock to XCLK. Do not rely on the
     * state of LCD_CAM after the failed camera init/deinit sequence. */
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_1_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = XCLK_FREQ_HZ,
        .clk_cfg = LEDC_USE_APB_CLK,
    };
    err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Diagnostic XCLK timer setup failed: %s", esp_err_to_name(err));
        return;
    }
    timer_ready = true;

    const ledc_channel_config_t channel = {
        .gpio_num = CAMERA_PIN_XCLK,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 1,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Diagnostic XCLK output setup failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    channel_ready = true;
    ESP_LOGI(TAG, "Temporary XCLK configured: GPIO%d, %d Hz (verify waveform externally)",
             CAMERA_PIN_XCLK, XCLK_FREQ_HZ);
    vTaskDelay(pdMS_TO_TICKS(100));

    const i2c_config_t bus = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = CAMERA_PIN_SIOD,
        .scl_io_num = CAMERA_PIN_SIOC,
        /* Use external pull-ups to sensor IOVDD, not MCU 3.3V pull-ups. */
        .sda_pullup_en = GPIO_PULLUP_DISABLE,
        .scl_pullup_en = GPIO_PULLUP_DISABLE,
        .master.clk_speed = 100000,
    };
    err = i2c_param_config(DIAG_I2C_PORT, &bus);
    if (err == ESP_OK) {
        err = i2c_driver_install(DIAG_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Diagnostic SCCB setup failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    bus_ready = true;
    vTaskDelay(pdMS_TO_TICKS(10));

    const int sda = gpio_get_level(CAMERA_PIN_SIOD);
    const int scl = gpio_get_level(CAMERA_PIN_SIOC);
    ESP_LOGI(TAG, "Bus idle digital levels: SDA(GPIO%d)=%d, SCL(GPIO%d)=%d",
             CAMERA_PIN_SIOD, sda, CAMERA_PIN_SIOC, scl);
    if (!sda || !scl) {
        ESP_LOGE(TAG, "Bus is held LOW. Check shorts, sensor power and external pull-ups before probing");
        goto cleanup;
    }

    err = probe_address(GC2145_ADDRESS);
    ESP_LOGI(TAG, "Probe 7-bit address 0x3C: %s", esp_err_to_name(err));
    if (err == ESP_OK) {
        uint8_t high = 0, low = 0;
        const esp_err_t high_err = read_id_register(0xf0, &high);
        const esp_err_t low_err = read_id_register(0xf1, &low);
        ESP_LOGI(TAG, "ID register F0: %s, value=0x%02x; F1: %s, value=0x%02x",
                 esp_err_to_name(high_err), high, esp_err_to_name(low_err), low);
        if (high_err == ESP_OK && low_err == ESP_OK) {
            const unsigned pid = ((unsigned)high << 8) | low;
            ESP_LOGI(TAG, "Sensor PID=0x%04x (expected 0x2145)", pid);
            if (pid == 0x2145) {
                ESP_LOGW(TAG, "GC2145 responds with temporary XCLK and extra settling time; inspect startup clock/reset timing");
            } else {
                ESP_LOGW(TAG, "Address responds but ID differs; verify sensor model and signal integrity");
            }
        }
    } else if (err == ESP_FAIL) {
        ESP_LOGW(TAG, "No ACK at 0x3C; scanning other non-reserved 7-bit addresses");
        int found = 0;
        for (uint8_t address = 0x08; address <= 0x77; ++address) {
            if (address == GC2145_ADDRESS) {
                continue;
            }
            err = probe_address(address);
            if (err == ESP_OK) {
                ++found;
                ESP_LOGI(TAG, "Address ACK: 0x%02x", address);
            } else if (err != ESP_FAIL) {
                ESP_LOGE(TAG, "Scan stopped at 0x%02x: %s", address, esp_err_to_name(err));
                goto cleanup;
            }
        }
        if (found == 0) {
            ESP_LOGE(TAG, "No SCCB device ACK. Check RESETB high, PWDN low, power rails, common GND and SDA/SCL wiring");
        }
    } else {
        ESP_LOGE(TAG, "Probe error: %s; check a stuck bus or controller error", esp_err_to_name(err));
    }

cleanup:
    if (bus_ready) {
        i2c_driver_delete(DIAG_I2C_PORT);
    }
    if (channel_ready) {
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    }
    if (timer_ready) {
        ledc_timer_pause(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0);
        timer.deconfigure = true;
        ledc_timer_config(&timer);
    }
    ESP_LOGI(TAG, "Diagnostics finished; temporary XCLK stopped. Power-cycle after any hardware changes");
}
