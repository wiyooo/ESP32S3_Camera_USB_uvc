#include "esp32_s3_szp.h"

#include "driver/ledc.h"
#include "esp_camera.h"
#include "esp_log.h"

static const char *TAG = "gc2145_camera";

esp_err_t bsp_camera_init(void)
{
#if CAMERA_OUTPUT_USB_UVC
    ESP_LOGI(TAG, "GC2145 USB UVC firmware: probe diagnostics v1");
#else
    ESP_LOGI(TAG, "GC2145 web camera firmware: probe diagnostics v1");
#endif
    const camera_config_t config = {
        .pin_pwdn = CAMERA_PIN_PWDN,
        .pin_reset = CAMERA_PIN_RESET,
        .pin_xclk = CAMERA_PIN_XCLK,
        /* Camera driver owns SCCB. No PCA9557 is fitted on this board. */
        .pin_sccb_sda = CAMERA_PIN_SIOD,
        .pin_sccb_scl = CAMERA_PIN_SIOC,
        .pin_d0 = CAMERA_PIN_D0,
        .pin_d1 = CAMERA_PIN_D1,
        .pin_d2 = CAMERA_PIN_D2,
        .pin_d3 = CAMERA_PIN_D3,
        .pin_d4 = CAMERA_PIN_D4,
        .pin_d5 = CAMERA_PIN_D5,
        .pin_d6 = CAMERA_PIN_D6,
        .pin_d7 = CAMERA_PIN_D7,
        .pin_vsync = CAMERA_PIN_VSYNC,
        .pin_href = CAMERA_PIN_HREF,
        .pin_pclk = CAMERA_PIN_PCLK,
        .xclk_freq_hz = XCLK_FREQ_HZ,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        /* GC2145 has no JPEG encoder. The selected output module compresses RGB565. */
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size = FRAMESIZE_QVGA,
        .jpeg_quality = 0, /* Not used for RGB565 capture. */
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_DRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s. Check power, RESETB, XCLK and SCCB wiring.",
                 esp_err_to_name(err));
        if (err == ESP_ERR_NOT_SUPPORTED) {
            /* esp32-camera 2.1.7 releases its SCCB bus on probe failure. */
            bsp_camera_probe_diagnostics();
        }
        return err;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL || sensor->id.PID != GC2145_PID) {
        ESP_LOGE(TAG, "Expected GC2145 (PID 0x2145), got 0x%04x",
                 sensor == NULL ? 0 : sensor->id.PID);
        esp_camera_deinit();
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "GC2145 ready: RGB565 %dx%d, XCLK %d Hz, one internal RAM buffer",
             CAMERA_FRAME_WIDTH, CAMERA_FRAME_HEIGHT, XCLK_FREQ_HZ);
    return ESP_OK;
}
