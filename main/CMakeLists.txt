option(CAMERA_OUTPUT_USB_UVC "Build USB UVC output instead of Wi-Fi web output" ON)

set(camera_sources
    "esp32_s3_szp.c"
    "camera_diagnostics.c"
    "main.c")
set(camera_private_requires
    esp32-camera
    esp_driver_ledc
    esp_driver_gpio
    driver)

if(CAMERA_OUTPUT_USB_UVC)
    list(APPEND camera_sources "usb_camera.c")
    list(APPEND camera_private_requires usb_device_uvc)
    set(camera_output_usb_uvc 1)
else()
    list(APPEND camera_sources "web_camera.c")
    list(APPEND camera_private_requires
         esp_http_server
         esp_wifi
         esp_event
         esp_netif
         nvs_flash)
    set(camera_output_usb_uvc 0)
endif()

idf_component_register(SRCS ${camera_sources}
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES ${camera_private_requires})

target_compile_definitions(${COMPONENT_LIB} PRIVATE
                           CAMERA_OUTPUT_USB_UVC=${camera_output_usb_uvc})
