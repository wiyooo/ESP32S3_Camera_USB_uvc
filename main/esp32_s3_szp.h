#pragma once

#include "esp_err.h"
#include "sdkconfig.h"

/* CAM_SCH_20260825_2.pdf: GPIO numbers, NOT QFN package pin numbers. */
#define CAMERA_PIN_SIOD     4
#define CAMERA_PIN_SIOC     5
#define CAMERA_PIN_VSYNC    6
#define CAMERA_PIN_HREF     7   /* CAM_HSYNC / GC2145 HSYNC */
#define CAMERA_PIN_D0       11
#define CAMERA_PIN_D1       9
#define CAMERA_PIN_D2       8
#define CAMERA_PIN_D3       10
#define CAMERA_PIN_D4       12
#define CAMERA_PIN_D5       18
#define CAMERA_PIN_D6       17
#define CAMERA_PIN_D7       16
#define CAMERA_PIN_PCLK     13
#define CAMERA_PIN_XCLK     15
#define CAMERA_PIN_PWDN     (-1) /* R1 holds PWDN low; no MCU connection. */
#define CAMERA_PIN_RESET    (-1) /* RESETB has no MCU connection in the schematic. */
#define XCLK_FREQ_HZ        24000000

#define CAMERA_FRAME_WIDTH  320
#define CAMERA_FRAME_HEIGHT 240
#define CAMERA_FRAME_RATE   20

/* CMake selects 0: Wi-Fi MJPEG web page or 1: USB UVC (default). */
#ifndef CAMERA_OUTPUT_USB_UVC
#define CAMERA_OUTPUT_USB_UVC 1
#endif

#if !CONFIG_IDF_TARGET_ESP32S3 || !CONFIG_GC2145_SUPPORT
#error "This project requires ESP32-S3 and the GC2145 sensor driver"
#endif
#if CAMERA_OUTPUT_USB_UVC && \
    (!CONFIG_FORMAT_MJPEG_CAM1 || !CONFIG_FRAMESIZE_QVGA || \
     CONFIG_UVC_CAM1_MULTI_FRAMESIZE || CONFIG_UVC_SUPPORT_TWO_CAM || \
     CONFIG_UVC_CAM1_FRAMERATE != CAMERA_FRAME_RATE)
#error "USB UVC mode requires one MJPEG QVGA camera"
#endif
esp_err_t bsp_camera_init(void);
void bsp_camera_probe_diagnostics(void);
esp_err_t app_camera_web_init(void);
esp_err_t app_camera_usb_init(void);
