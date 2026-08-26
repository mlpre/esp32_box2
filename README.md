# ATK-DNESP32S3-BOX2-WIFI 通用驱动与硬件测试

工程分为根目录下可复用的 ESP-IDF 驱动组件 `driver` 和独立的硬件测试应用 `main`，适配
`xiaozhi-esp32` PR #954 中的 `ATK-DNESP32S3-BOX2-WIFI`。驱动不依赖测试面板、提示音、
Wi-Fi 扫描或临时测试文件，可以直接复制到其他 ESP-IDF 工程使用。

## 驱动库调用

将 `driver` 复制到目标工程根目录，并通过 `EXTRA_COMPONENT_DIRS` 注册；在应用组件的
`CMakeLists.txt` 中加入 `PRIV_REQUIRES driver`，然后通过聚合头文件调用：

```c
#include "box2.h"

ESP_ERROR_CHECK(box2_board_init());
ESP_ERROR_CHECK(box2_motion_init(box2_board_i2c_bus()));

box2_motion_state_t motion = {0};
ESP_ERROR_CHECK(box2_motion_read(&motion));
```

公开接口按硬件拆分：

- `box2_board_*`：共享 I2C、TCA9555、按键和电池状态
- `box2_audio_*`：PCM 采集/播放、输出音量和输入增益
- `box2_lcd_*`：LCD 初始化、背光和 RGB565 位图绘制
- `box2_motion_*`：SC7A20 初始化和三轴采样
- `box2_storage_*`：TF 卡挂载、容量刷新、挂载点和卸载

`main/hardware_test_utils.*` 中的提示音、麦克风峰值、SD 读写校验，以及
`main/hardware_test_screen.*` 中的色条和测试面板，均属于测试层，不会链接进驱动库。

`box2_board_power_off()` 会按照 BOX2 的硬件时序释放 `SYS_POW` 电源锁存；该接口应仅在
确认设备由电池供电时调用。USB 接入时外部电源不会被 `SYS_POW` 切断。

## 已覆盖的硬件

| 硬件 | 测试方式 |
|---|---|
| ESP32-S3 / 16 MB Flash / 8 MB OPI PSRAM | 启动时读取容量并显示 |
| I2C（GPIO48/47） | 扫描总线，预期发现 ES8389 `0x10`、SC7A20 `0x19` 和 TCA9555 `0x20` |
| TCA9555 扩展 IO | 配置并回读电源、扬声器、USB、按键和充电状态引脚 |
| SC7A20 三轴加速度计 | 校验 ID `0x11`，配置 50 Hz 高精度模式，成组读取 X/Y/Z 并判断六个方向 |
| ST7789 240×320 并口屏 | 显示 RGB 色条和实时测试面板 |
| LCD 背光（GPIO21） | 启动时执行三档 PWM 亮度变化 |
| ES8389 + I2S | 24 kHz 双工初始化、播放两段提示音、持续显示去除直流偏置后的麦克风峰值 |
| L/Q/M/R 四个按键 | 上电自动标定空闲电平，同时显示按下状态和原始电平 |
| 充电状态 | 显示 TCA9555 充电状态输入 |
| 电池 ADC（GPIO1 / ADC1_CH0） | 每 5 秒采样，按上游 BOX2 标定表估算电压和电量 |
| 2.4 GHz Wi-Fi | 主动扫描周围 AP，显示数量和最强信号，串口列出前 10 个 |
| TF/MicroSD 卡 | SPI3 挂载 FAT 文件系统，显示卡名、容量、剩余空间并执行文件写入回读删除测试 |

测试初始化时写入 `BOX2_XIO_SAFE_OUTPUTS`，保持 `SYS_POW` 电源锁存，并回读确认
TCA9555 输出状态。

显示使用 5×7 单倍点阵字体和 PSRAM 全屏帧缓冲，在 240×320 竖屏中同时显示 17 行
硬件状态。测试面板和麦克风电平条每 400 ms 一起刷新。

SC7A20、ES8389 和 TCA9555 共用 I2C 总线，7 位地址分别为 `0x19`、`0x10` 和 `0x20`。
TF 卡使用 SPI3：SCLK GPIO17、MOSI GPIO16、MISO GPIO18、CS GPIO15，测试频率为
25 MHz。TF 卡应在上电前插入，并使用 FAT/FAT32 文件系统；测试不会格式化卡，临时
文件 `/sdcard/box2_test.tmp` 会在校验完成后自动删除。

## 编译与烧录

本工程仅使用 ESP-IDF 6.0.2，并已完成完整编译验证。
工程目标是 ESP32-S3 N16R8：16 MB Flash、8 MB OPI PSRAM，控制台使用 USB Serial/JTAG。
`sdkconfig.defaults` 已将默认构建目标设为 `esp32s3`。

### 1. 准备 ESP-IDF 环境

打开已配置好 ESP-IDF 6.0.2 环境的 PowerShell，进入项目根目录，然后执行：

```powershell
idf.py --version
```

如果尚未配置环境，请先按照 ESP-IDF 安装说明启动对应版本的 PowerShell。成功后，
`idf.py --version` 应显示 `ESP-IDF v6.0.2`。

### 2. 首次配置和编译

干净源码中没有 `sdkconfig`、`dependencies.lock`、`managed_components` 和 `build`，执行：

```powershell
idf.py build
```

构建过程会自动完成以下工作：

1. 从 `sdkconfig.defaults` 选择 ESP32-S3 并生成 `sdkconfig`。
2. 根据 `driver/idf_component.yml` 解析并下载 `espressif/esp_codec_dev`，同时生成 `dependencies.lock`。
3. 在 `build` 中生成 Bootloader、分区表和应用固件。

成功时最后会出现 `Project build complete`。主要输出为：

| 文件 | 烧录地址 | 作用 |
|---|---:|---|
| `build/bootloader/bootloader.bin` | `0x0` | 启动加载程序 |
| `build/partition_table/partition-table.bin` | `0x8000` | Flash 分区表 |
| `build/esp32_box2.bin` | `0x10000` | 测试应用程序 |

### 3. 查找串口并烧录

查看 Windows 串口：

```powershell
Get-PnpDevice -Class Ports | Format-Table Status,FriendlyName
```

将下面的 `COM7` 换成实际端口：

```powershell
idf.py -p COM7 flash monitor
```

这个命令会自动按正确地址烧录三个镜像，然后打开串口日志。退出监视器按 `Ctrl+]`。

如果无法自动进入下载模式，按住 BOOT、短按 RESET、松开 BOOT，再重新执行烧录命令。
更换过分区布局或旧固件异常时，可以先完整擦除：

```powershell
idf.py -p COM7 erase-flash
idf.py -p COM7 flash monitor
```

### 4. 生成可从 0x0 直接刷写的单文件

普通 `idf.py build` 生成的是三个分区镜像。如果需要乐鑫 Flash Download Tool 或单文件备份，
在构建成功后执行：

```powershell
esptool --chip esp32s3 merge-bin `
  --flash-mode dio --flash-freq 80m --flash-size 16MB `
  -o .\build\esp32_box2_full.bin `
  0x0 .\build\bootloader\bootloader.bin `
  0x8000 .\build\partition_table\partition-table.bin `
  0x10000 .\build\esp32_box2.bin
```

生成的 `build/esp32_box2_full.bin` 应从地址 `0x0` 烧录。命令行直接写入示例：

```powershell
esptool --chip esp32s3 --port COM7 --baud 460800 write-flash `
  --flash-mode dio --flash-freq 80m --flash-size 16MB `
  0x0 .\build\esp32_box2_full.bin
```

### 5. 清理和重新构建

删除普通编译结果：

```powershell
idf.py fullclean
```

需要完全回到干净源码状态时，还可以删除自动生成的 `build`、`managed_components`、
`sdkconfig` 和 `dependencies.lock`；再次执行第 2 步即可恢复。不要删除
`sdkconfig.defaults` 或 `driver/idf_component.yml`。

## 上电现象和操作

1. 背光从暗到亮变化，屏幕显示 RGB 色条。
2. 扬声器播放两段提示音。
3. 屏幕进入 17 行实时测试面板；串口打印 I2C、音频、Wi-Fi、电池、按键、加速度和 TF 卡详情。
4. 对麦克风说话，底部绿色电平条应变化。
5. L/Q/M/R 键分别播放 440/660/880/1040 Hz。上电标定期间不要按住按键。
6. 转动开发板，`ACC X/Y/Z` 和 `ORI` 应随姿态改变。
7. 插有可写 TF 卡时，`SD OK`、`RW=OK`、容量和剩余空间应正常显示。

面板中的 `OK` 表示驱动调用和可回读状态正常。LCD 色彩、背光变化、扬声器声音、
麦克风电平和实体按键仍需人工观察，这些项目无法仅靠固件自动判定。

## 主要文件

- `driver/*.h`：稳定的公开 API 和板级配置
- `driver/*.c`：音频、板级 IO、LCD、运动和存储驱动实现
- `main/hardware_test_utils.c`：提示音、麦克风峰值和 TF 卡读写测试
- `main/hardware_test_screen.c`：测试专用色条和实时状态面板
- `main/main.c`：启动自检、Wi-Fi 扫描和交互循环

## 干净源码目录

本工程交付时只保留下面这些源码和可复现配置：

```text
esp32_box2/
├─ .gitignore
├─ CMakeLists.txt
├─ README.md
├─ sdkconfig.defaults
├─ driver/
│  ├─ CMakeLists.txt
│  ├─ idf_component.yml
│  ├─ box2.h
│  ├─ box2_config.h
│  ├─ box2_board.c
│  ├─ box2_board.h
│  ├─ box2_audio.c
│  ├─ box2_audio.h
│  ├─ box2_motion.c
│  ├─ box2_motion.h
│  ├─ box2_storage.c
│  ├─ box2_storage.h
│  ├─ box2_lcd.c
│  └─ box2_lcd.h
└─ main/
   ├─ CMakeLists.txt
   ├─ main.c
   ├─ hardware_test_utils.c
   ├─ hardware_test_utils.h
   ├─ hardware_test_screen.c
   └─ hardware_test_screen.h
```

`build/`、`managed_components/`、`sdkconfig` 和 `dependencies.lock` 都是构建时自动生成的内容，
不属于源码交付物。

硬件定义来源：[xiaozhi-esp32 PR #954](https://github.com/78/xiaozhi-esp32/pull/954/changes)。
