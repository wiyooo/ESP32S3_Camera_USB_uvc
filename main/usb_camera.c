#include <assert.h>
#include <stdlib.h>

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "img_converters.h"
#include "usb_device_uvc.h"

#include "esp32_s3_szp.h"

#define UVC_WIDTH                  320
#define UVC_HEIGHT                 240
#define UVC_MAX_JPEG_SIZE          (75 * 1024)
#define UVC_JPEG_QUALITY           80
#define ZOOM_BUTTON_GPIO           GPIO_NUM_0
#define ZOOM_TASK_PERIOD_MS        20
#define ZOOM_DEBOUNCE_MS           60

static const char *TAG = "usb_camera";

typedef struct {
    camera_fb_t *camera_fb;
    uint8_t *jpeg_buf;
    uvc_fb_t uvc_fb;
} usb_camera_frame_t;

static usb_camera_frame_t s_frame;
static uint8_t *s_uvc_transfer_buffer;
static uint8_t *s_zoom_rgb565_buffer;
static volatile bool s_streaming;
static volatile bool s_zoom_enabled;

static void rgb565_center_zoom_2x(const camera_fb_t *source, uint8_t *destination)
{
    const uint16_t *src = (const uint16_t *)source->buf;
    uint16_t *dst = (uint16_t *)destination;
    const size_t crop_x = source->width / 4;
    const size_t crop_y = source->height / 4;

    /* Nearest-neighbour 2x scaling of the central 160x120 region. */
    for (size_t y = 0; y < source->height; ++y) {
        const size_t src_y = crop_y + (y / 2);
        for (size_t x = 0; x < source->width; ++x) {
            const size_t src_x = crop_x + (x / 2);
            dst[y * source->width + x] = src[src_y * source->width + src_x];
        }
    }
}

static void zoom_button_task(void *arg)
{
    (void)arg;
    bool stable_level = true;
    bool last_sample = true;
    TickType_t changed_at = xTaskGetTickCount();

    while (true) {
        const bool sample = gpio_get_level(ZOOM_BUTTON_GPIO) != 0;
        const TickType_t now = xTaskGetTickCount();

        if (sample != last_sample) {
            last_sample = sample;
            changed_at = now;
        } else if (sample != stable_level &&
                   (now - changed_at) >= pdMS_TO_TICKS(ZOOM_DEBOUNCE_MS)) {
            stable_level = sample;
            if (!stable_level) {
                s_zoom_enabled = !s_zoom_enabled;
                ESP_LOGI(TAG, "Digital zoom: %s", s_zoom_enabled ? "2x" : "1x");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(ZOOM_TASK_PERIOD_MS));
    }
}

static esp_err_t usb_camera_start(uvc_format_t format, int width, int height,
                                  int rate, void *ctx)
{
    (void)ctx;
    if (format != UVC_FORMAT_JPEG || width != UVC_WIDTH || height != UVC_HEIGHT) {
        ESP_LOGE(TAG, "Unsupported UVC mode: format=%d, %dx%d @ %d FPS",
                 format, width, height, rate);
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_streaming = true;
    ESP_LOGI(TAG, "UVC stream started: MJPEG %dx%d @ %d FPS", width, height, rate);
    return ESP_OK;
}

static void usb_camera_stop(void *ctx)
{
    (void)ctx;
    s_streaming = false;
    ESP_LOGI(TAG, "UVC stream stopped");
}

static uvc_fb_t *usb_camera_fb_get(void *ctx)
{
    (void)ctx;
    if (!s_streaming) {
        return NULL;
    }

    s_frame.camera_fb = esp_camera_fb_get();
    if (s_frame.camera_fb == NULL) {
        ESP_LOGW(TAG, "Camera frame capture failed");
        return NULL;
    }

    camera_fb_t jpeg_source = *s_frame.camera_fb;
    if (s_zoom_enabled) {
        rgb565_center_zoom_2x(s_frame.camera_fb, s_zoom_rgb565_buffer);
        jpeg_source.buf = s_zoom_rgb565_buffer;
        jpeg_source.len = UVC_WIDTH * UVC_HEIGHT * sizeof(uint16_t);
    }

    size_t jpeg_len = 0;
    s_frame.jpeg_buf = NULL;
    if (!frame2jpg(&jpeg_source, UVC_JPEG_QUALITY,
                   &s_frame.jpeg_buf, &jpeg_len)) {
        ESP_LOGE(TAG, "RGB565 to JPEG conversion failed");
        esp_camera_fb_return(s_frame.camera_fb);
        s_frame.camera_fb = NULL;
        return NULL;
    }

    if (jpeg_len > UVC_MAX_JPEG_SIZE) {
        ESP_LOGE(TAG, "JPEG frame too large: %u bytes (maximum %u)",
                 (unsigned)jpeg_len, (unsigned)UVC_MAX_JPEG_SIZE);
        free(s_frame.jpeg_buf);
        s_frame.jpeg_buf = NULL;
        esp_camera_fb_return(s_frame.camera_fb);
        s_frame.camera_fb = NULL;
        return NULL;
    }

    s_frame.uvc_fb.buf = s_frame.jpeg_buf;
    s_frame.uvc_fb.len = jpeg_len;
    s_frame.uvc_fb.width = s_frame.camera_fb->width;
    s_frame.uvc_fb.height = s_frame.camera_fb->height;
    s_frame.uvc_fb.format = UVC_FORMAT_JPEG;
    s_frame.uvc_fb.timestamp = s_frame.camera_fb->timestamp;
    return &s_frame.uvc_fb;
}

static void usb_camera_fb_return(uvc_fb_t *fb, void *ctx)
{
    (void)ctx;
    assert(fb == &s_frame.uvc_fb);
    free(s_frame.jpeg_buf);
    s_frame.jpeg_buf = NULL;
    if (s_frame.camera_fb != NULL) {
        esp_camera_fb_return(s_frame.camera_fb);
        s_frame.camera_fb = NULL;
    }
}

esp_err_t app_camera_usb_init(void)
{
    const gpio_config_t button_config = {
        .pin_bit_mask = BIT64(ZOOM_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&button_config), TAG, "BOOT button init failed");

    s_zoom_rgb565_buffer = heap_caps_malloc(UVC_WIDTH * UVC_HEIGHT * sizeof(uint16_t),
                                            MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (s_zoom_rgb565_buffer == NULL) {
        ESP_LOGE(TAG, "Unable to allocate digital zoom buffer");
        return ESP_ERR_NO_MEM;
    }

    s_uvc_transfer_buffer = heap_caps_malloc(UVC_MAX_JPEG_SIZE,
                                             MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (s_uvc_transfer_buffer == NULL) {
        ESP_LOGE(TAG, "Unable to allocate the %u-byte USB transfer buffer",
                 (unsigned)UVC_MAX_JPEG_SIZE);
        free(s_zoom_rgb565_buffer);
        s_zoom_rgb565_buffer = NULL;
        return ESP_ERR_NO_MEM;
    }

    uvc_device_config_t config = {
        .uvc_buffer = s_uvc_transfer_buffer,
        .uvc_buffer_size = UVC_MAX_JPEG_SIZE,
        .start_cb = usb_camera_start,
        .fb_get_cb = usb_camera_fb_get,
        .fb_return_cb = usb_camera_fb_return,
        .stop_cb = usb_camera_stop,
        .cb_ctx = NULL,
    };

    esp_err_t err = uvc_device_config(0, &config);
    if (err == ESP_OK) {
        err = uvc_device_init();
    }
    if (err != ESP_OK) {
        free(s_uvc_transfer_buffer);
        s_uvc_transfer_buffer = NULL;
        free(s_zoom_rgb565_buffer);
        s_zoom_rgb565_buffer = NULL;
        return err;
    }

    if (xTaskCreate(zoom_button_task, "zoom_button", 2048, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Unable to create BOOT button task");
        uvc_device_deinit();
        free(s_uvc_transfer_buffer);
        s_uvc_transfer_buffer = NULL;
        free(s_zoom_rgb565_buffer);
        s_zoom_rgb565_buffer = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "USB webcam ready on GPIO19 (D-) / GPIO20 (D+)");
    return ESP_OK;
}
