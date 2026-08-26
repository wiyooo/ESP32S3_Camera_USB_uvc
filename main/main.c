#include "esp32_s3_szp.h"

void app_main(void)
{
    ESP_ERROR_CHECK(bsp_i2c_init());
    pca9557_init();
    bsp_camera_init();
    ESP_ERROR_CHECK(app_camera_usb_init());
}
