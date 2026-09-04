#include "esp32_s3_szp.h"

#if !CAMERA_OUTPUT_USB_UVC

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_camera.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "img_converters.h"
#include "nvs_flash.h"

#define WIFI_STA_SSID             "ZTE-P6FGRP"
#define WIFI_STA_PASSWORD         "28545478"
#define WIFI_CONNECT_MAX_RETRY    10
#define WIFI_CONNECT_TIMEOUT_MS   30000
#define WIFI_CONNECTED_BIT        BIT0
#define WIFI_FAILED_BIT           BIT1
#define WEB_MAX_JPEG_SIZE         (40 * 1024)
#define WEB_JPEG_QUALITY          60
#define FRAME_RETRY_DELAY_MS      20

static const char *TAG = "web_camera";
static const char STREAM_TYPE[] = "multipart/x-mixed-replace; boundary=frame";
static const char STREAM_BOUNDARY[] = "--frame\r\n";
static const char STREAM_PART_HEADER[] =
    "Content-Type: image/jpeg\r\n"
    "Content-Length: %u\r\n\r\n";

static const char INDEX_HTML[] =
    "<!doctype html><html lang=\"zh-CN\"><head>"
    "<meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>GC2145 Camera</title><style>"
    "*{box-sizing:border-box}body{margin:0;background:#111;color:#eee;font-family:Arial,sans-serif;"
    "display:flex;min-height:100vh;align-items:center;justify-content:center}"
    "main{width:min(94vw,760px);text-align:center}h1{font-size:1.35rem;margin:.5rem}"
    "p{color:#aaa;margin:.4rem 0 1rem}img{display:block;width:100%;height:auto;"
    "background:#000;border:1px solid #333;border-radius:10px;image-rendering:auto}"
    "</style></head><body><main><h1>GC2145 Camera</h1>"
    "<p>320 &times; 240 MJPEG</p><img src=\"/stream\" alt=\"Camera stream\">"
    "</main></body></html>";

typedef struct {
    uint8_t *buffer;
    size_t length;
    bool overflow;
} jpeg_output_t;

static jpeg_output_t s_jpeg;
static SemaphoreHandle_t s_stream_mutex;
static EventGroupHandle_t s_wifi_event_group;
static httpd_handle_t s_http_server;
static esp_netif_t *s_sta_netif;
static esp_event_handler_instance_t s_wifi_event_instance;
static esp_event_handler_instance_t s_ip_event_instance;
static bool s_wifi_event_registered;
static bool s_ip_event_registered;
static bool s_wifi_initialized;
static bool s_wifi_started;
static bool s_initialized;
static int s_wifi_retry_count;
static esp_ip4_addr_t s_sta_ip;

/* frame2jpg_cb() does not treat a callback short write as a hard failure.
 * Latch overflow here and discard the whole frame after encoding. */
static size_t jpeg_write(void *ctx, size_t index, const void *data, size_t len)
{
    jpeg_output_t *output = ctx;
    if (output->overflow) {
        return 0;
    }
    if (index != output->length || index > WEB_MAX_JPEG_SIZE ||
        len > WEB_MAX_JPEG_SIZE - index || (len != 0 && data == NULL)) {
        output->overflow = true;
        return 0;
    }
    if (len != 0) {
        memcpy(output->buffer + index, data, len);
    }
    output->length += len;
    return len;
}

static esp_err_t encode_next_frame(size_t *jpeg_length)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        return ESP_ERR_TIMEOUT;
    }

    if (fb->format != PIXFORMAT_RGB565 || fb->width != CAMERA_FRAME_WIDTH ||
        fb->height != CAMERA_FRAME_HEIGHT ||
        fb->len != CAMERA_FRAME_WIDTH * CAMERA_FRAME_HEIGHT * 2) {
        ESP_LOGW(TAG, "Invalid camera frame: format=%d, %ux%u, %u bytes",
                 fb->format, (unsigned)fb->width, (unsigned)fb->height,
                 (unsigned)fb->len);
        esp_camera_fb_return(fb);
        return ESP_ERR_INVALID_SIZE;
    }

    s_jpeg.length = 0;
    s_jpeg.overflow = false;
    bool encoded = frame2jpg_cb(fb, WEB_JPEG_QUALITY, jpeg_write, &s_jpeg);
    esp_camera_fb_return(fb);

    if (!encoded || s_jpeg.overflow || s_jpeg.length < 4 ||
        s_jpeg.buffer[0] != 0xff || s_jpeg.buffer[1] != 0xd8 ||
        s_jpeg.buffer[s_jpeg.length - 2] != 0xff ||
        s_jpeg.buffer[s_jpeg.length - 1] != 0xd9) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *jpeg_length = s_jpeg.length;
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "HTTP GET /");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    return httpd_resp_send(req, INDEX_HTML, sizeof(INDEX_HTML) - 1);
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    if (xSemaphoreTake(s_stream_mutex, 0) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "A camera stream is already active.");
    }

    ESP_LOGI(TAG, "HTTP GET /stream: stream started");

    esp_err_t result = httpd_resp_set_type(req, STREAM_TYPE);
    if (result == ESP_OK) {
        result = httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    }
    if (result == ESP_OK) {
        result = httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    }

    unsigned dropped_frames = 0;
    while (result == ESP_OK) {
        size_t jpeg_length = 0;
        esp_err_t frame_err = encode_next_frame(&jpeg_length);
        if (frame_err != ESP_OK) {
            ++dropped_frames;
            if (dropped_frames == 1 || dropped_frames % 30 == 0) {
                ESP_LOGW(TAG, "Frame dropped (%u total): %s", dropped_frames,
                         esp_err_to_name(frame_err));
            }
            vTaskDelay(pdMS_TO_TICKS(FRAME_RETRY_DELAY_MS));
            continue;
        }

        char part_header[80];
        int header_length = snprintf(part_header, sizeof(part_header),
                                     STREAM_PART_HEADER, (unsigned)jpeg_length);
        if (header_length < 0 || header_length >= (int)sizeof(part_header)) {
            result = ESP_FAIL;
            break;
        }

        result = httpd_resp_send_chunk(req, STREAM_BOUNDARY,
                                       sizeof(STREAM_BOUNDARY) - 1);
        if (result == ESP_OK) {
            result = httpd_resp_send_chunk(req, part_header, header_length);
        }
        if (result == ESP_OK) {
            result = httpd_resp_send_chunk(req, (const char *)s_jpeg.buffer,
                                           jpeg_length);
        }
        if (result == ESP_OK) {
            result = httpd_resp_send_chunk(req, "\r\n", 2);
        }
    }

    xSemaphoreGive(s_stream_mutex);
    ESP_LOGI(TAG, "Browser stream disconnected");
    return result;
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    return err;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = event_data;
        if (s_initialized || s_wifi_retry_count < WIFI_CONNECT_MAX_RETRY) {
            ++s_wifi_retry_count;
            ESP_LOGW(TAG, "Wi-Fi disconnected (reason=%d), retry %d",
                     event->reason, s_wifi_retry_count);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "Could not connect to Wi-Fi after %d retries",
                     WIFI_CONNECT_MAX_RETRY);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAILED_BIT);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        s_sta_ip = event->ip_info.ip;
        s_wifi_retry_count = 0;
        ESP_LOGI(TAG, "Wi-Fi connected, DHCP IP: " IPSTR, IP2STR(&s_sta_ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t start_wifi_station(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        return err;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_init);
    if (err != ESP_OK) {
        return err;
    }
    s_wifi_initialized = true;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, NULL,
                                              &s_wifi_event_instance);
    if (err != ESP_OK) {
        return err;
    }
    s_wifi_event_registered = true;

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              wifi_event_handler, NULL,
                                              &s_ip_event_instance);
    if (err != ESP_OK) {
        return err;
    }
    s_ip_event_registered = true;

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_STA_SSID,
            .password = WIFI_STA_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err == ESP_OK) {
        s_wifi_started = true;
        err = esp_wifi_set_ps(WIFI_PS_NONE);
    }
    if (err != ESP_OK) {
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if ((bits & WIFI_CONNECTED_BIT) != 0) {
        return ESP_OK;
    }
    if ((bits & WIFI_FAILED_BIT) != 0) {
        return ESP_FAIL;
    }
    ESP_LOGE(TAG, "Wi-Fi connection timed out after %d ms", WIFI_CONNECT_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.core_id = 1;
    config.max_uri_handlers = 2;
    config.max_open_sockets = 2;
    config.lru_purge_enable = false;
    config.send_wait_timeout = 5;

    esp_err_t err = httpd_start(&s_http_server, &config);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = NULL,
    };

    err = httpd_register_uri_handler(s_http_server, &index_uri);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(s_http_server, &stream_uri);
    }
    return err;
}

static void release_web_resources(void)
{
    if (s_http_server != NULL) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }
    if (s_wifi_event_registered) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              s_wifi_event_instance);
        s_wifi_event_registered = false;
    }
    if (s_ip_event_registered) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              s_ip_event_instance);
        s_ip_event_registered = false;
    }
    if (s_wifi_started) {
        esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_wifi_initialized) {
        esp_wifi_deinit();
        s_wifi_initialized = false;
    }
    if (s_sta_netif != NULL) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    if (s_wifi_event_group != NULL) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }
    if (s_stream_mutex != NULL) {
        vSemaphoreDelete(s_stream_mutex);
        s_stream_mutex = NULL;
    }
    free(s_jpeg.buffer);
    s_jpeg.buffer = NULL;
}

esp_err_t app_camera_web_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_jpeg.buffer = heap_caps_malloc(WEB_MAX_JPEG_SIZE,
                                     MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    s_stream_mutex = xSemaphoreCreateMutex();
    if (s_jpeg.buffer == NULL || s_stream_mutex == NULL) {
        ESP_LOGE(TAG, "Not enough internal RAM for the web JPEG buffer");
        release_web_resources();
        return ESP_ERR_NO_MEM;
    }

    jpgSetRgb565BE(true);
    esp_err_t err = init_nvs();
    if (err == ESP_OK) {
        err = start_wifi_station();
    }
    if (err == ESP_OK) {
        err = start_http_server();
    }
    if (err != ESP_OK) {
        release_web_resources();
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Camera web page ready");
    ESP_LOGI(TAG, "Wi-Fi SSID: %s", WIFI_STA_SSID);
    ESP_LOGI(TAG, "Open http://" IPSTR "/ in a browser", IP2STR(&s_sta_ip));
    ESP_LOGI(TAG, "GC2145 stream: RGB565 -> JPEG, %dx%d, one browser at a time",
             CAMERA_FRAME_WIDTH, CAMERA_FRAME_HEIGHT);
    ESP_LOGI(TAG, "Free internal RAM: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return ESP_OK;
}

#endif /* !CAMERA_OUTPUT_USB_UVC */
