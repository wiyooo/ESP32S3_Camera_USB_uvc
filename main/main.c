#include "esp32_s3_szp.h"

#include "esp_camera.h"
#include "esp_log.h"

void app_main(void)
{
    esp_err_t err = bsp_camera_init();
    if (err != ESP_OK) {
        /* Leave UART diagnostics available instead of continuously rebooting. */
        return;
    }

#if CAMERA_OUTPUT_USB_UVC
    err = app_camera_usb_init();
#else
    err = app_camera_web_init();
#endif
    if (err != ESP_OK) {
        ESP_LOGE("main", "Camera output init failed: %s", esp_err_to_name(err));
        esp_camera_deinit();
    }
}
