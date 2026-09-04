# GC2145 USB UVC 摄像头

**当前源码已追加 probe diagnostics v1（仅在识别失败时执行）。按用户要求，这次只修改源码，没有编译或烧录；build 内已有 bin 不包含该诊断，需要自行重新编译。**

在原 07-lcd_camera 工程上修改，保留 ESP-IDF 5.4.1、esp32-camera 2.1.7 和 usb_device_uvc 1.3.1。
运行链路只有：GC2145 DVP -> RGB565 -> 软件 JPEG -> USB OTG UVC -> 电脑摄像头应用。
没有 LCD、按键变焦、PCA9557、姿态传感器、音频、Wi-Fi 或蓝牙应用功能。
原有图片资源头文件保留在目录内，但没有引用，也不进入固件。

## 先核对硬件

依据 CAM_SCH_20260825_2.pdf 检查，以下问题不能仅靠本程序解决。PDF 只有原理图，尚未核验 PCB 和实物：

1. **USB 网名没有接通，且正负标注反了。** J1 经 R9/R10 后的网名是 D-/D+，U5 侧是 USB_D+/USB_D-，它们不是相同网络。图中 U5 GPIO19 标成 USB_D+、GPIO20 标成 USB_D-。正确连接应为 J1.2 -> R9 -> GPIO19（D-，U5.25），J1.3 -> R10 -> GPIO20（D+，U5.26）。请用网表和万用表核对实际连通性，不要只按现有网名接线。
2. **摄像头 RESETB 悬空。** U4.C2 仅接 RST 网名，没有 MCU 引脚或上拉。本程序按图设置 pin_reset=-1，无法驱动该信号。硬件必须提供满足 GC2145 数据手册的复位电平和上电时序；若后续接到 GPIO，再修改 CAMERA_PIN_RESET。PWDN 则已由 R1 下拉，pin_pwdn=-1。
3. **摄像头电源器件型号与电压网名不一致。** U1、U2 都标为 ME6231C33，而电源网名分别是 2V8_CAM 和 1V8_CAM。C33 是 3.3V 输出型号，不能靠改网名获得 2.8V/1.8V。请核对 BOM、实际料号和实测电压，再给摄像头上电；同时核对 IOVDD=2.8V 时 SCCB 的 3.3V 上拉和 MCU 输出信号是否满足传感器电气限制。
4. **Flash / MCU 完整型号。** Flash 按用户确认的实物容量 **16MB** 配置，原理图 U6 的 PY25Q64 标注与实物不一致，后续应同步修改原理图/BOM。请核对完整料号及 VDD_SPI 电压匹配。U5 只标 ESP32_QFN56，需确认实际芯片确实是 ESP32-S3；本程序不适用于不带原生 USB 的普通 ESP32。

ESP32-S3 USB 引脚依据：[乐鑫 USB Serial/JTAG 文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/usb-serial-jtag-console.html)。
GC2145 输出格式依据：[乐鑫 esp32-camera](https://github.com/espressif/esp32-camera)。
ME6231 型号依据：[微盟 ME6231 数据手册，第 2 页](https://uploadcdn.oneyac.com/attachments/files/brand_pdf/microne/58/0E/MICRONE-ME6231C33M5G.pdf)。

## 摄像头引脚

表中都是 GPIO 编号，不是 QFN 封装脚号。定义在 main/esp32_s3_szp.h。

| 信号 | ESP32-S3 GPIO |
| --- | --- |
| SDA / SIOD | 4 |
| SCL / SIOC | 5 |
| VSYNC | 6 |
| HSYNC / HREF | 7 |
| D0 | 11 |
| D1 | 9 |
| D2 | 8 |
| D3 | 10 |
| D4 | 12 |
| D5 | 18 |
| D6 | 17 |
| D7 | 16 |
| PCLK | 13 |
| MCLK / XCLK | 15（24MHz） |
| USB D- | 19 |
| USB D+ | 20 |

驱动自行初始化 SCCB，不需要先初始化板级 I2C，不再访问 PCA9557。

## 视频配置与内存

- 电脑端设备名称：ESP32-S3 GC2145 Camera。
- USB 模式：USB 2.0 Full Speed、UVC、MJPEG、Isochronous。
- 唯一视频模式：320x240，描述符标称 20 FPS。实际帧率受曝光和软件压缩速度影响。
- GC2145 不支持原生 JPEG，故采集 RGB565，再以质量 80 软件压缩；USB 使用 Bulk 传输以提高有效吞吐。
- 不依赖 PSRAM：一个 153600 字节内部 RAM 原始帧缓冲，以及一个由 JPEG 编码与 USB 发送共用的 64KiB 零拷贝缓冲；另需驱动 DMA、编码工作区及任务内存。
- USB 的 UVC 任务按主机请求取帧并编码，没有额外采集队列或变焦任务。缓冲复用，超限/损坏帧丢弃，避免发送截断 JPEG。
- 应用日志只走 UART0（115200），不会占用 UVC 的 USB 口。
- main 中相机初始化失败即返回，并保留串口错误信息；这时不会枚举 UVC 摄像头。

## 编译和烧录

在 ESP-IDF 5.4.1 PowerShell 终端进入此工程；普通 PowerShell 可先执行本机的导出脚本：

```powershell
. D:/v5.4.1/esp-idf/export.ps1
cd C:/Users/wiyo/Desktop/07-lcd_camera
idf.py build
idf.py -p COM实际端口 flash
```

COM实际端口必须替换为本次下载模式下设备管理器显示的端口，不要直接使用旧 VS Code 配置的 COM19。

USB 硬件连接修正后，可使用 J1 连接电脑：1=5V，2=D-，3=D+，4=GND。
按住 K2（BOOT），按一下 K1（CHIP_EN），再松开 K2，进入 ROM 下载模式。
烧录后按 K1 复位运行；若烧录工具提示需要手动复位，按提示操作。
运行时该 USB 口只有摄像头功能，没有应用 USB 串口。再次烧录时重复 BOOT/RESET 操作。
USB 线必须支持数据传输，不要通过 J2 两针供电口传视频。

本次已生成合并固件 build/gc2145_uvc_16mb_merged.bin，包含 bootloader、分区表和应用，整份烧录地址为 **0x0**。不要将单独的 build/lcd_camera.bin 烧到 0x0；它的地址是 0x10000。合并文件包含中间填充区，整包烧录会覆盖这些区域中的旧数据（包括 NVS），有需保留的数据时使用上面的 idf.py flash 分文件烧录方式。

在已激活的 ESP-IDF 终端中，可重新生成合并固件：

```powershell
idf.py build
idf.py merge-bin -o gc2145_uvc_16mb_merged.bin
```

合并输出在 build 目录内；更改源码后需要重新生成，不能沿用旧 bin。

也可用 3.3V 电平 USB-UART 适配器：适配器 TX -> TP1/U0_RX，RX -> TP2/U0_TX，GND -> TP3。
不要将适配器 5V 信号接到 UART 引脚；避免 USB 与外部电源互相倒灌。
本次没有自动连接串口、烧录芯片或修改 eFuse。

## 电脑免驱预览

Windows 使用系统 UVC 驱动，无需安装自定义摄像头驱动。打开系统“相机”并切换到此摄像头；也可以在 OBS 中添加“视频采集设备”，选择上述设备，分辨率 320x240、MJPEG、20 FPS。
“免驱”指使用系统自带 UVC 驱动，不是插入 USB 后自动弹出画面。不要用 Zadig 把 UVC 接口替换成 WinUSB/libusb。

## 上板验收

1. 先确认 USB 连线、RESETB、电源电压和芯片完整料号。
2. UART0 应显示 Detected GC2145 camera、GC2145 ready、GC2145 USB camera ready。
3. 电脑设备管理器识别 UVC 摄像头，预览应有实时画面和正确颜色。
4. 连续开关预览、拔插 USB、重新上电，确认可再次出图；观察是否有帧超时、编码超限或堆/栈错误。
5. 实测帧率、长时间稳定性和传感器时序仍需硬件验证；编译通过不能替代此步骤。

## 首次视频版本验证记录（2026-09-01，不含后续诊断修改）

- ESP-IDF 5.4.1：reconfigure、build、size 通过；最终构建日志 build/gc2145_build.log 中没有 warning/error。
- 烧录参数核对：ESP32-S3、16MB、DIO、40MHz；应用 272960 字节，可放入默认 1MB 应用分区。16MB 是整颗 Flash 容量，不等于程序文件必须有 16MB。
- ELF 符号检查：仅包含 GC2145 检测实现，没有 LCD、PCA9557、QMI8658、变焦任务或 Wi-Fi/蓝牙初始化函数。
- 主机测试 tests/native_uvc_smoke.py 使用实际 usb_camera.c 和组件 JPEG 编码器；验证边界保护、错误帧归还、初始化失败清理及多次启停回调。模拟测试不覆盖真实 USB 总线的插拔或枚举。
- 生成色条与随机噪声 JPEG，使用 Pillow 解码确认尺寸、RGB 颜色顺序及 64KiB 缓冲边界。
- Xtensa 编译器静态栈报告已检查；JPEG 转换函数自身栈为 1168 字节。实际任务栈水位仍需上板确认。

主机测试需要 Python、Pillow、GCC/G++，不参与设备固件编译：

```powershell
python tests/native_uvc_smoke.py --cc D:/MinGW/bin/gcc.exe --cxx D:/MinGW/bin/g++.exe
```

尚未在实物上烧录或验证 UVC 出图；必须先完成上面的硬件核对。

## 摄像头识别失败诊断

当前用户日志停在 Camera probe failed / ESP_ERR_NOT_SUPPORTED，尚未进入 UVC 初始化。该错误也可能表示地址没有应答，不等于没有 GC2145 驱动。

main/camera_diagnostics.c 在这类失败后接管已经释放的 SCCB 总线，临时以 LEDC 在 GPIO15 输出配置为 20MHz 的时钟，等待 100ms，再诊断一次。诊断只选择/读取芯片 ID 寄存器，不修改传感器配置；退出时释放 I2C 并停止临时时钟，不会自动恢复视频。

重新编译并烧录当前源码后，应先看到 GC2145 UVC firmware: probe diagnostics v1。若没有这行，先核对是否烧录了旧 bin，而不是继续根据旧日志判断硬件。

| 诊断日志 | 含义 / 下一步 |
| --- | --- |
| Bus idle digital levels: SDA=0 或 SCL=0 | 总线被拉低；查供电、短路、外部上拉和焊接。GPIO 数字电平不能替代电压测量。 |
| Probe 7-bit address 0x3C: ESP_FAIL | 目标地址没有 ACK；结合后续地址扫描和实测电压检查 RESETB、PWDN、MCLK、SDA/SCL。 |
| Probe 7-bit address 0x3C: ESP_ERR_TIMEOUT | 总线忙/超时，需要检查波形，不能直接认定为摄像头型号错误。 |
| Address ACK: 0xXX | 其他地址有设备响应，核对实际器件型号与接线，不自动把该设备当作 GC2145。 |
| ID register F0/F1 读失败 | 地址响应但读寄存器失败，查信号质量、复位/时钟和器件型号；该行的数值仅在 ESP_OK 时有效。 |
| Sensor PID=0x2145 | 临时时钟及额外等待下成功读到 GC2145，下一步检查正常启动的时钟/复位时序。 |
| Sensor PID 为其他值 | 先排查传感器实际型号及通信质量，不能直接认为摄像头损坏。 |
| No SCCB device ACK | 未找到响应设备；检查供电、RESETB 高电平、PWDN 低电平、共地以及 SDA/SCL 的物理连接。 |

诊断使用外部上拉，关闭 ESP32 内部 3.3V 上拉。请确认实际外部上拉连接到适合传感器的 IOVDD。程序无法通过未连接的 RESETB 引脚控制复位，RESETB/电源实测值仍必须由硬件侧确认。
