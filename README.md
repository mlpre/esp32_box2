# ATK-DNESP32S3-BOX2-WIFI 全硬件测试 Demo

这是一个独立的 ESP-IDF 板级测试程序，适配 `xiaozhi-esp32` PR #954 中的
`ATK-DNESP32S3-BOX2-WIFI`。它不包含小智业务逻辑，只用于确认板载硬件和引脚连接。

## 已覆盖的硬件

| 硬件 | 测试方式 |
|---|---|
| ESP32-S3 / 16 MB Flash / 8 MB OPI PSRAM | 启动时读取容量并显示 |
| I2C（GPIO48/47） | 扫描总线，预期发现 ES8389 `0x10` 和 TCA9555 `0x20` |
| TCA9555 扩展 IO | 配置并回读电源、扬声器、USB、按键和充电状态引脚 |
| ST7789 240×320 并口屏 | 显示 RGB 色条和实时测试面板 |
| LCD 背光（GPIO21） | 启动时执行三档 PWM 亮度变化 |
| ES8389 + I2S | 24 kHz 双工初始化、播放两段提示音、持续显示去除直流偏置后的麦克风峰值 |
| L/Q/M/R 四个按键 | 上电自动标定空闲电平，同时显示按下状态和原始电平 |
| 充电状态 | 显示 TCA9555 充电状态输入 |
| 电池 ADC（GPIO1 / ADC1_CH0） | 每 5 秒采样，按上游 BOX2 标定表估算电压和电量 |
| 2.4 GHz Wi-Fi | 主动扫描周围 AP，显示数量和最强信号，串口列出前 10 个 |

测试不会拉低 `SYS_POW`，也不会打开 `VBUS_EN`，避免 demo 在巡检中自行关机或向
USB 口反向供电。

显示刷新采用静态文字整帧更新、麦克风电平条局部更新，屏幕方向固定为 240×320 竖屏，
避免文字闪跳、覆盖和长行裁切。

## 编译与烧录

本工程仅使用 ESP-IDF 6.0.2，并已完成完整编译验证。
工程目标是 ESP32-S3 N16R8：16 MB Flash、8 MB OPI PSRAM，控制台使用 USB Serial/JTAG。

### 1. 准备 ESP-IDF 环境

打开 PowerShell，进入工程并加载 ESP-IDF 6.0.2 环境：

```powershell
cd C:\Users\ML\Documents\esp32_box2
. C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1
idf.py --version
```

如果使用其他安装位置，应改为对应的 ESP-IDF 6.0.2 PowerShell 环境脚本。成功后，
`idf.py --version` 应显示 `ESP-IDF v6.0.2`。

### 2. 首次配置和编译

干净源码中没有 `sdkconfig`、`dependencies.lock`、`managed_components` 和 `build`，执行：

```powershell
idf.py set-target esp32s3
idf.py build
```

构建过程会自动完成以下工作：

1. 根据 `sdkconfig.defaults` 生成 `sdkconfig`。
2. 根据 `main/idf_component.yml` 解析并下载 `espressif/esp_codec_dev`，同时生成 `dependencies.lock`。
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
`sdkconfig.defaults` 或 `main/idf_component.yml`。

## 上电现象和操作

1. 背光从暗到亮变化，屏幕显示 RGB 色条。
2. 扬声器播放两段提示音。
3. 屏幕进入实时测试面板；串口打印 I2C、音频、Wi-Fi、电池和按键详情。
4. 对麦克风说话，底部绿色电平条应变化。
5. L/Q/M/R 键分别播放 440/660/880/1040 Hz。上电标定期间不要按住按键。

面板中的 `OK` 表示驱动调用和可回读状态正常。LCD 色彩、背光变化、扬声器声音、
麦克风电平和实体按键仍需人工观察，这些项目无法仅靠固件自动判定。

## 主要文件

- `main/box2_config.h`：完整板级引脚定义
- `main/box2_board.c`：I2C、TCA9555、按键、电池 ADC
- `main/box2_lcd.c`：ST7789、8080 总线、背光和测试 UI
- `main/box2_audio.c`：ES8389、I2S、扬声器和麦克风
- `main/main.c`：启动自检、Wi-Fi 扫描和交互循环

## 干净源码目录

本工程交付时只保留下面这些源码和可复现配置：

```text
esp32_box2/
├─ .gitignore
├─ CMakeLists.txt
├─ README.md
├─ sdkconfig.defaults
└─ main/
   ├─ CMakeLists.txt
   ├─ idf_component.yml
   ├─ main.c
   ├─ box2_config.h
   ├─ box2_board.c
   ├─ box2_board.h
   ├─ box2_audio.c
   ├─ box2_audio.h
   ├─ box2_lcd.c
   └─ box2_lcd.h
```

`build/`、`managed_components/`、`sdkconfig` 和 `dependencies.lock` 都是构建时自动生成的内容，
不属于源码交付物。

硬件定义来源：[xiaozhi-esp32 PR #954](https://github.com/78/xiaozhi-esp32/pull/954/changes)。
