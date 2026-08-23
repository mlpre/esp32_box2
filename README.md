# NEON RUSH — ESP32-S3 体感赛车

面向 ATK-DNESP32S3-BOX2-WIFI 的单机体感赛车游戏。游戏直接使用板载
SC7A20 加速度计、240×320 ST7789 屏幕、L/Q/M/R 四个按键和 ES8389
音频 Codec，不依赖 Wi-Fi 或 TF 卡。

## 游戏内容

- 伪 3D 透视赛道、动态弯道、滚动路肩、山脉、树木和多色交通车辆
- 分层夕阳天空、云层、城市灯光、护栏、霓虹灯柱、路面纹理和高速残影
- 带阴影、车窗反光、尾灯、尾翼和排气火焰的多层车辆造型
- 三分区霓虹 HUD、立体菜单面板与发光倒计时界面
- 左右倾斜转向，前后倾斜调节车速
- 3 秒起跑倒计时、超车计分、碰撞/生命值、最高分和重新开始
- 超级简单难度：仅 2 辆障碍车、低车速、轻碰撞伤害和较长无敌时间
- 高灵敏度转向：20 mg 小死区，轻微倾斜即可产生明显响应
- 随车速变化的实时合成引擎声
- 起跑、菜单、超车、碰撞和比赛结束音效
- 0–100% 音量，10% 一档，开机默认 50%
- PSRAM 全屏帧缓冲配合 DMA 刷新，游戏逻辑与音频播放互不阻塞

## 操作

| 操作 | 功能 |
|---|---|
| 左右倾斜设备 | 转向 |
| 向前/向后倾斜 | 加速/减速 |
| M 键短按 | 开始、暂停、继续；结束后再来一局 |
| M 键长按 2 秒 | 关机 |
| L 键 | 音量降低 10% |
| R 键 | 音量提高 10% |
| Q 键 | 重新开始 |

开机后的约 0.5 秒会自动标定加速度计中位。此时请按正常游玩姿势平稳握住
设备。根据 BOX2 实机的传感器安装方向，屏幕左右转向固定使用 SC7A20 的 Y 轴，
无需进行轴向选择。标题页显示 `STEERING Y FIXED` 后按 M 开始。HUD 顶部显示
车速、分数、音量、生命值和实时倾斜指示。

长按 M 两秒后会显示关机画面、停止音频并释放 `SYS_POW` 电源锁存。如果仍连接
USB，设备会进入深度睡眠；重新上电或按复位键可再次启动。

## 硬件配置

| 硬件 | 配置 |
|---|---|
| MCU | ESP32-S3，16 MB Flash，8 MB OPI PSRAM，240 MHz |
| 屏幕 | ST7789，240×320，8 位 8080 并口，GPIO21 PWM 背光 |
| 体感 | SC7A20，I2C 地址 `0x19`，100 Hz 采样 |
| 音频 | ES8389，24 kHz I2S，板载扬声器 |
| 按键 | L/Q/M 经 TCA9555，R 为 GPIO0 |

详细引脚定义位于 `main/box2_config.h`。

## 编译与烧录

工程使用 ESP-IDF 6.0.2，目标必须是 ESP32-S3：

```powershell
cd C:\Users\ML\Documents\esp32_box2
. C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1
idf.py set-target esp32s3
idf.py build
idf.py -p COM7 flash monitor
```

将 `COM7` 换成设备的实际串口。当前源码已经通过 ESP-IDF 6.0.2 完整编译；
应用镜像输出到 `build/esp32_box2.bin`。

## 主要文件

- `main/main.c`：游戏状态机、体感控制、车辆生成、碰撞与计分
- `main/box2_lcd.c`：伪 3D 赛道、车辆、HUD 和菜单渲染
- `main/box2_audio.c`：非阻塞引擎声与音效合成、音量控制
- `main/box2_motion.c`：SC7A20 驱动和 mg 换算
- `main/box2_board.c`：TCA9555、四按键和板级电源初始化
- `main/box2_config.h`：完整板级引脚配置

## 烧录输出

完整烧录由 `idf.py flash` 自动完成。对应镜像地址为：

| 文件 | 地址 |
|---|---:|
| `build/bootloader/bootloader.bin` | `0x0` |
| `build/partition_table/partition-table.bin` | `0x8000` |
| `build/esp32_box2.bin` | `0x10000` |
