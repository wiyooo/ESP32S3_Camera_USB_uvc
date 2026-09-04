# GC2145 网页摄像头

当前源码只在应用入口启用网页视频，USB UVC 的源文件、组件依赖和配置均已保留；UVC 实现由预处理开关屏蔽，运行时不会初始化 UVC。
按用户要求，本次没有编译、烧录或更新 `build` 目录；其中现有固件仍是旧版本，必须自行重新编译后再烧录。

## 使用方式

摄像头初始化成功后，ESP32-S3 会连接指定的 2.4GHz Wi-Fi：

| 项目 | 值 |
| --- | --- |
| Wi-Fi 名称 | `ZTE-P6FGRP` |
| Wi-Fi 密码 | 固化在 `main/web_camera.c` 中 |
| 网页地址 | 由路由器 DHCP 分配，以串口打印为准 |
| 视频格式 | MJPEG，320x240 |

电脑或手机也连接 `ZTE-P6FGRP`，然后打开串口日志中 `Open http://.../ in a browser` 打印的 **HTTP** 地址。地址由路由器分配，重启后可能变化，不再使用 `192.168.4.1`。网页只有实时画面，没有 LCD、按键、音频、姿态传感器或其他业务功能。若路由器启用了访客网络或 AP/客户端隔离，需关闭隔离，否则同一 Wi-Fi 下的设备也无法互相访问。

GC2145 本身不输出 JPEG。程序按以下链路工作：

`GC2145 DVP RGB565 -> 软件 JPEG -> HTTP multipart MJPEG -> 浏览器`

程序使用一个 40KiB 可复用 JPEG 缓冲区，JPEG 质量为 60。为避免同一个缓冲区被并发覆盖，当前按一个浏览器视频连接设计。复杂画面若压缩结果超过 40KiB，会丢弃该帧，不会发送截断图片。由于板上没有 PSRAM，Wi-Fi/LwIP 缓冲数量已按单客户端视频用途下调，为摄像头和 HTTP 请求保留内部 RAM。

## 输出模式开关

输出模式由 `main/esp32_s3_szp.h` 中这一行决定：

```c
#define CAMERA_OUTPUT_USB_UVC 0
```

- `0`：当前的 Wi-Fi 网页视频。
- `1`：恢复 USB UVC 初始化。

切换为 `1` 后，`main/usb_camera.c`、`espressif/usb_device_uvc` 和 QVGA/20FPS UVC 配置会继续使用，不需要重新找回旧代码。两种输出不会同时启动。

## 自行编译

在 ESP-IDF 5.4.1 PowerShell 环境中进入工程后执行：

```powershell
cd C:/Users/wiyo/Desktop/07-lcd_camera
idf.py build
idf.py -p COM实际端口 flash monitor
```

成功启动时，串口应依次出现类似日志：

```text
GC2145 ready: RGB565 320x240 ...
Camera web page ready
Wi-Fi connected, DHCP IP: 192.168.x.x
Wi-Fi SSID: ZTE-P6FGRP
Open http://192.168.x.x/ in a browser
```

Flash 配置仍为用户确认的 16MB、DIO、40MHz。网页和保留的 UVC 组件会增加固件体积，因此 `partitions.csv` 已将 factory 应用分区设为 4MB；其余 Flash 暂不分配。摄像头初始化失败时不会启动热点和网页，原有 probe diagnostics 仅在失败路径运行，正常采集时不会执行。
