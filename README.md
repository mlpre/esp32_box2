# ATK-DNESP32S3-BOX2-WIFI 网络收音机

这是一个面向 ATK-DNESP32S3-BOX2-WIFI 的第一版独立网络收音机。应用不使用
ESP Player、ESP-GMF 或 ESP-ADF 播放管线，也不包含 TF/SD 卡功能。

数据链路由应用直接控制：

```text
Radio Browser M3U directory (startup only)
        |
        v
HTTP/HTTPS MP3 stream
        |
        v
esp_http_client blocking reader
        |
        v
MP3 frame parser + low-level decoder
        |
        v
stereo to mono mix + linear resampler to 24 kHz
        |
        v
box2_audio_write -> ES8389 -> speaker
```

底层 MP3 解码使用 `espressif/esp_audio_codec` 2.6.2，只调用其解码器接口；网络读取、
电台状态机、切台、重连、声道混合、重采样和音频输出均由本工程实现。界面采用
LVGL 9.2.2 排版，并由工程内的 `radio_font.bin` 独立提供中文点阵，不依赖
任何外部字体组件。

## 第一版功能

- 启动时从 Radio Browser 加载最多 1000 个中国地区 MP3 电台
- 目录只保存在 PSRAM，不使用 TF/SD 卡
- 在线目录失败时每 5 秒自动重试，不使用写死的备用电台
- HTTP 和 HTTPS 拉流
- 证书包校验 HTTPS 服务端
- MP3 任意分块输入和帧同步
- 单声道/双声道输入兼容
- 任意常见 MP3 采样率线性重采样到板载音频的 24kHz
- 断流后 2.5 秒自动重连，连续失败两次后自动切到下一台
- 切台时终止当前连接并打开新电台
- 16px、1bpp 中文界面，内置 28191 个 Unicode 码位
- 覆盖基本汉字、CJK 扩展 A、中文标点、全角字符、ASCII 和 Latin-1
- 中文台名支持 UTF-8，并可在屏幕上换行显示
- 屏幕以中文显示连接状态、IP、音频格式、接收字节数、音量和电量
- 实体按键切台与调节音量

在线目录按热度排序，并过滤目录中已标记失效的电台和重复项。实际加载数量由
Radio Browser 当时的目录数据决定，最多为 1000 个。

网络不可用或目录服务失败时，屏幕会显示“电台目录加载失败，稍后重试”。联网成功
并取得至少一个有效 MP3 电台后才开始播放。

## 按键

| 按键 | 功能 |
|---|---|
| L | 上一个电台 |
| Q | 音量降低 5% |
| M | 音量提高 5% |
| R | 下一个电台 |

## 配置 Wi-Fi

工程不会把 Wi-Fi 密码写入源码。进入配置菜单：

```powershell
idf.py menuconfig
```

打开 `BOX2 Internet Radio`，填写：

- `Wi-Fi SSID`
- `Wi-Fi password`

SSID 为空时固件仍可启动并显示配置提示，但不会启动网络拉流。

## 编译和烧录

工程固定使用 ESP-IDF 6.0.2，目标为 ESP32-S3 N16R8：16MB Flash、8MB OPI PSRAM。
本地字库约 881KiB；应用分区预留为 6MB，便于继续增加电台功能。

```powershell
idf.py build
idf.py -p COM5 flash monitor
```

请将 `COM5` 换成设备实际串口。退出监视器按 `Ctrl+]`。

本版本已完成 ESP-IDF 6.0.2 全量构建验证。应用镜像约 2.17MiB，6MB 应用分区
剩余约 64%。生成文件为：

- `build/bootloader/bootloader.bin`，烧录地址 `0x0`
- `build/partition_table/partition-table.bin`，烧录地址 `0x8000`
- `build/esp32_box2.bin`，烧录地址 `0x10000`

## 主要文件

- `main/main.c`：板级初始化、Wi-Fi、按键和 UI 调度
- `main/radio_font.c`：工程自带字库的 LVGL 字形接口
- `main/radio_font.bin`：随固件链接的 16px 中文点阵数据
- `main/radio_stream.c`：在线目录、HTTP 拉流、MP3 解码、重采样及重连
- `main/radio_screen.c`：LVGL 中文网络收音机界面
- `main/Kconfig.projbuild`：Wi-Fi 配置项
- `driver/box2_audio.c`：ES8389 与 I2S PCM 输出
- `driver/box2_board.c`：按键和电池状态
- `driver/box2_lcd.c`：ST7789 屏幕输出

字库数据由 `tools/generate_radio_font.py` 从 GNU Unifont 17.0.04 的官方 HEX
文件生成。字体采用 SIL Open Font License 1.1，版权和完整许可见
`LICENSES/UNIFONT-OFL-1.1.txt`。

## 当前限制

- 只播放 MP3 直播流，不支持 AAC、HLS 或 PLS；M3U 仅用于启动时读取在线目录。
- 在线目录每次启动重新获取，不做本地持久化。
- 字库覆盖 Unicode BMP 内的常用中文区域；BMP 之外的扩展汉字会显示为 `?`。
- 未解析 ICY 歌曲标题；请求中明确关闭了 ICY metadata。
- 音频输出固定为单声道 24kHz。
- 电台切换最迟会受到 5 秒 HTTP 读取超时影响。
- 没有 TF/SD 卡缓存、收藏或离线播放功能。
