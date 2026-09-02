# BOX-2 Wi-Fi 扩展屏

本项目把 **ATK-DNESP32S3-BOX2-WIFI** 变成一块 Windows 11 无线扩展屏。

项目包含两部分：

- **ESP32-S3 固件**：连接 2.4 GHz Wi-Fi，接收 MJPEG 画面并显示到 BOX-2 的 320×240 LCD。
- **Windows 主机端**：安装一个 480×360、30 Hz 的虚拟显示器，抓取并缩放该显示器画面，编码为 JPEG 后通过局域网发送给 BOX-2。

画面只通过 Wi-Fi 传输。USB 串口仅用于固件烧录、首次配网和日志调试，不传输显示画面。

## 主要功能

- Windows 11 扩展显示器，固定横屏分辨率为 480×360，最高 30 帧每秒
- BOX-2 LCD 原生显示分辨率为 320×240
- JPEG 质量为 80，发送前会把放大的 Windows 鼠标指针合成到画面中
- 自动发现同一局域网中的 BOX-2，无需手动填写设备 IP
- 视频使用 UDP 5000 端口，设备发现使用 UDP 5001 端口
- 丢包时跳过不完整帧，优先显示最新完整帧，避免延迟不断累积
- Wi-Fi 或 BOX-2 短暂重连后自动恢复串流
- 长按中间 M 键 1.5 秒可关机或待机
- `driver` 目录提供可独立复用的 BOX-2 板级驱动

## 工作流程

```text
Windows 虚拟显示器（480×360）
          ↓ 抓屏并缩放
Windows 主机程序（320×240、JPEG 质量 80）
          ↓ 局域网 UDP 分片
BOX-2 固件（JPEG 解码为 RGB565）
          ↓
BOX-2 LCD（320×240）
```

完整部署顺序如下：

1. 构建并烧录 ESP32-S3 固件。
2. 通过 USB 串口写入 Wi-Fi 名称和密码。
3. 构建并安装 Windows 显示驱动及主机程序。
4. 在 Windows 显示设置中选择“扩展这些显示器”。

## 环境要求

### 硬件

- ATK-DNESP32S3-BOX2-WIFI
- USB 数据线
- 2.4 GHz Wi-Fi 路由器或接入点
- Windows 11 x64 电脑

电脑和 BOX-2 必须能在同一局域网中互相访问。访客网络、无线客户端隔离、不同 VLAN 或严格的防火墙策略可能导致自动发现失败。

### ESP32 固件构建环境

- ESP-IDF 6.0.2
- Python、CMake、Ninja 和乐鑫工具链（由 ESP-IDF 安装器一并配置）

本工程固定使用 ESP-IDF 6.0.2，`driver/idf_component.yml` 也对版本进行了约束，不建议直接使用其他版本。

### Windows 主机端构建环境

- Windows 11 x64
- Visual Studio 2022 Build Tools
- “使用 C++ 的桌面开发”工作负载及 v143 工具集
- Windows Driver Kit（WDK）10.0.26100 或更高版本

Windows 驱动当前只提供 x64 构建配置，不支持 32 位或 ARM64 Windows。

## 一、构建 ESP32 固件

### 1. 打开 ESP-IDF 环境

从开始菜单打开 **ESP-IDF 6.0.2 PowerShell**，先进入项目根目录，然后检查版本：

```powershell
idf.py --version
```

版本输出应包含 `ESP-IDF v6.0.2`。如果普通 PowerShell 找不到 `idf.py`，请使用安装器创建的 ESP-IDF PowerShell，或者先执行对应版本的 `export.ps1`。

### 2. 编译

```powershell
idf.py build
```

首次构建会自动：

1. 根据 `sdkconfig.defaults` 选择 ESP32-S3、16 MB Flash 和 8 MB OPI PSRAM。
2. 下载 `espressif/esp_codec_dev` 和 `espressif/esp_new_jpeg` 组件。
3. 生成 `sdkconfig`、`dependencies.lock`、`managed_components` 和 `build`。
4. 编译 Bootloader、分区表和应用固件。

看到 `Project build complete` 表示构建成功。主要产物如下：

| 文件 | 默认烧录地址 | 说明 |
|---|---:|---|
| `build/bootloader/bootloader.bin` | `0x0` | 启动加载程序 |
| `build/partition_table/partition-table.bin` | `0x8000` | 分区表 |
| `build/esp32_box2.bin` | `0x10000` | BOX-2 应用固件 |

实际烧录参数以构建后生成的 `build/flash_args` 为准。

## 二、烧录固件并完成首次配网

### 1. 确认串口

连接 BOX-2 后查看串口：

```powershell
Get-PnpDevice -Class Ports | Format-Table Status,FriendlyName
```

记下设备对应的端口号，例如 `COM7`。

### 2. 烧录并打开串口监视器

把 `COM7` 替换为实际端口：

```powershell
idf.py -p COM7 flash monitor
```

如果设备无法自动进入下载模式，请按住 BOOT，短按 RESET，松开 BOOT，然后重试。退出串口监视器按 `Ctrl+]`。

### 3. 首次写入 Wi-Fi

固件第一次启动且 NVS 中没有网络信息时，串口会显示：

```text
BOX2_PROVISION_READY
Send WIFI_SSID:<ssid> and WIFI_PASS:<password> over USB serial.
```

依次输入以下两行，每行输入后按回车：

```text
WIFI_SSID:你的无线网络名称
WIFI_PASS:你的无线网络密码
```

成功后会看到：

```text
BOX2_SSID_ACCEPTED
BOX2_PASS_ACCEPTED
BOX2_IP=设备获得的IP地址
BOX2_UDP_READY=5000
```

注意：

- BOX-2 仅支持 2.4 GHz Wi-Fi。
- 当前固件要求 WPA2/WPA3 网络，密码至少 8 个字符，不支持开放网络。
- Wi-Fi 信息保存在 NVS 中，普通重新烧录应用不会清除，之后无需重复输入。
- 串口日志会打印网络名称和密码长度，但不会打印密码正文。

### 4. 修改已保存的 Wi-Fi

当前固件没有单独的清除网络命令。更换路由器或密码时，擦除 Flash 后重新烧录：

```powershell
idf.py -p COM7 erase-flash
idf.py -p COM7 flash monitor
```

擦除会同时删除固件和已保存的 Wi-Fi 信息，重启后需要重新配网。

## 三、构建 Windows 显示驱动

在项目根目录打开普通 Windows PowerShell，然后执行：

```powershell
cd .\windows\Box2Display
Set-ExecutionPolicy -Scope Process Bypass
.\build.ps1
```

`build.ps1` 会：

1. 使用 Visual Studio 2022 的 MSBuild 编译 `Box2DisplayHost.exe`。
2. 使用 WDK 编译 UMDF 间接显示驱动 `Box2Display.dll`。
3. 生成 INF 和 CAT 驱动包。
4. 创建或复用有效期五年的本地开发代码签名证书。
5. 对驱动包签名。

成功后的驱动包位于 `windows\Box2Display\out\Package`，包含 `Box2Display.inf`、`Box2Display.dll`、`Box2Display.cat` 和 `Box2Display.cer`。主机程序位于 `windows\Box2Display\out\Host\Box2DisplayHost.exe`。

如果提示找不到 MSBuild，请在 Visual Studio Installer 中安装 Visual Studio 2022 Build Tools 和 C++ 工作负载。如果提示找不到 `Inf2Cat.exe` 或 `signtool.exe`，请安装或修复 WDK。

## 四、安装 Windows 显示驱动

构建成功后，在 `windows\Box2Display` 目录执行：

```powershell
.\install.ps1
```

脚本会弹出管理员权限确认，然后：

- 将开发证书加入本机“受信任的根证书颁发机构”和“受信任的发布者”；
- 使用 `pnputil` 安装显示驱动；
- 把主机程序安装到 `C:\Program Files\BOX-2 Display`；
- 添加名为“BOX-2 Wi-Fi Display”的 Windows 防火墙入站规则；
- 创建名为 `Box2DisplayHost` 的登录计划任务并立即启动。

安装后打开“设置 → 系统 → 显示”。如果 Windows 没有自动启用新屏幕，请在“多显示器”中选择“扩展这些显示器”。虚拟显示器名称为 **BOX-2 Wi-Fi Display Adapter**，固定使用 480×360 横屏模式。

本项目使用本机自签名开发证书，只适合开发和本机测试。公开分发需要使用符合 Microsoft 要求的正式驱动签名。

## 五、日常使用

1. 打开 BOX-2，等待它连接 Wi-Fi；屏幕会先显示测试图和等待页面。
2. 登录 Windows 后，计划任务会以管理员权限启动主机程序。
3. 主机程序通过 UDP 5001 自动发现 BOX-2，连接成功后开始发送画面。
4. 在 Windows 显示设置中排列扩展屏位置，然后把窗口拖到该屏幕。

日常串流不要求连接 USB。只要 BOX-2 有电，并且与电脑处于可互通的同一局域网，就可以无线显示。

### M 键电源操作

- **电池供电**：长按中间 M 键 1.5 秒，关闭 LCD 并释放 `SYS_POW` 电源锁存，实现硬件关机；再次长按 M 键开机。
- **USB 供电**：USB 不受 `SYS_POW` 控制，长按 M 键会进入显示待机；短按一次 M 键可唤醒并恢复最新画面。

## 六、更新和清理

### 更新 ESP32 固件

```powershell
idf.py -p COM7 build flash monitor
```

普通更新会保留 NVS 中的 Wi-Fi 信息。

### 清理 ESP32 构建结果

```powershell
idf.py fullclean
```

`build`、`managed_components`、`sdkconfig` 和 `dependencies.lock` 都是自动生成内容。需要完全重新解析配置和依赖时可以删除它们，但不要删除 `sdkconfig.defaults`、`main/idf_component.yml` 或 `driver/idf_component.yml`。

### 更新 Windows 主机端或驱动

在 `windows\Box2Display` 中重新执行：

```powershell
.\build.ps1
.\install.ps1
```

安装脚本会停止旧主机进程、替换程序、覆盖计划任务并重新启动。

## 七、卸载 Windows 显示驱动

在 `windows\Box2Display` 目录执行：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\uninstall.ps1
```

脚本会请求管理员权限并删除虚拟显示设备、驱动包、计划任务、防火墙规则和 `C:\Program Files\BOX-2 Display` 中的程序。

卸载脚本不会自动删除已导入的开发证书。如不再需要，可在 Windows 证书管理器中手动删除主题为 `BOX-2 Display Development` 的证书。

## 八、生成单文件固件

普通构建会生成多个分区镜像。如需用于乐鑫 Flash Download Tool 或备份的单文件，在构建完成后执行：

```powershell
esptool --chip esp32s3 merge-bin `
  --flash-mode dio --flash-freq 80m --flash-size 16MB `
  -o .\build\esp32_box2_full.bin `
  0x0 .\build\bootloader\bootloader.bin `
  0x8000 .\build\partition_table\partition-table.bin `
  0x10000 .\build\esp32_box2.bin
```

生成的文件必须从 `0x0` 地址烧录：

```powershell
esptool --chip esp32s3 --port COM7 --baud 460800 write-flash `
  --flash-mode dio --flash-freq 80m --flash-size 16MB `
  0x0 .\build\esp32_box2_full.bin
```

## 九、常见问题

### BOX-2 一直停在等待页面

连接 USB 串口并查看日志：

```powershell
idf.py -p COM7 monitor
```

- 看到 `BOX2_PROVISION_READY`：尚未配网，请输入网络名称和密码。
- 反复看到 `Wi-Fi disconnected; reconnecting`：网络信息错误或接入点不兼容；擦除 Flash 后重新配网。
- 已看到 `BOX2_IP=...` 和 `BOX2_UDP_READY=5000`：固件联网正常，继续检查 Windows 主机、防火墙和局域网隔离。

### Windows 中没有出现 BOX-2 显示器

1. 确认 `build.ps1` 和 `install.ps1` 没有报错。
2. 在任务计划程序中确认 `Box2DisplayHost` 正在运行。
3. 在设备管理器的“显示适配器”中检查 `BOX-2 Wi-Fi Display Adapter`。
4. 打开“设置 → 系统 → 显示”，尝试“检测”并选择扩展显示。
5. 修改驱动后重新构建和安装，不要只替换 DLL。

### Windows 有虚拟显示器，但 BOX-2 没有画面

1. 确认电脑和 BOX-2 位于同一可互通局域网。
2. 禁用路由器的 AP 隔离、客户端隔离或访客网络隔离。
3. 确认 Windows 防火墙中存在启用状态的“BOX-2 Wi-Fi Display”规则。
4. 确认 UDP 5000 和 UDP 5001 没有被安全软件或网络策略拦截。
5. 串口日志出现 `UDP stream client accepted` 表示主机与固件握手成功。

### 画面卡顿或延迟

本项目使用 UDP，拥塞或丢包时会主动跳帧。建议使用信号较强、干扰较少的网络，避免访客网络和多级无线中继。串口每两秒输出一次 `STREAM_STATS`，其中 `rx_fps`、`lcd_fps` 和 `drop_fps` 可用于判断接收、显示和丢帧情况。

### 脚本因执行策略被阻止

只为当前 PowerShell 进程临时放行：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
```

关闭该窗口后，此设置自动失效。

## 十、项目目录

```text
esp32_box2/
├─ CMakeLists.txt                 ESP-IDF 工程入口
├─ sdkconfig.defaults             ESP32-S3、Flash、PSRAM 和网络默认配置
├─ driver/                        可复用的 BOX-2 板级驱动组件
│  ├─ box2_board.*                I2C、扩展 IO、按键、电池和电源控制
│  ├─ box2_audio.*                ES8389 音频采集与播放
│  ├─ box2_lcd.*                  ST7789 LCD 与背光控制
│  ├─ box2_motion.*               SC7A20 加速度计
│  └─ box2_storage.*              TF/MicroSD 卡
├─ main/
│  ├─ rgb_stream_main.c           当前固件入口：配网、发现、收流、解码和显示
│  ├─ hardware_test_screen.*      启动等待页面绘制
│  └─ main.c 等                   保留的硬件测试源码，当前构建未启用
└─ windows/Box2Display/
   ├─ Host/                       抓屏、缩放、JPEG 编码和 UDP 发送程序
   ├─ Driver/                     Windows 间接显示驱动
   ├─ build.ps1                   Windows 构建脚本
   ├─ install.ps1                 安装和更新脚本
   └─ uninstall.ps1               卸载脚本
```

当前参与固件构建的源码以 `main/CMakeLists.txt` 为准；旧硬件测试源码不会链接进当前 Wi-Fi 显示固件。

## 驱动组件复用

如需在其他 ESP-IDF 工程中复用 BOX-2 驱动，可复制 `driver` 目录，通过 `EXTRA_COMPONENT_DIRS` 注册，并在应用组件的 `CMakeLists.txt` 中加入 `PRIV_REQUIRES driver`：

```c
#include "box2.h"

ESP_ERROR_CHECK(box2_board_init());
ESP_ERROR_CHECK(box2_lcd_init());
ESP_ERROR_CHECK(box2_lcd_set_backlight(100));
```

公开接口按硬件划分：

- `box2_board_*`：共享 I2C、TCA9555、按键、电池和电源状态
- `box2_audio_*`：PCM 采集、播放、输出音量和输入增益
- `box2_lcd_*`：LCD 初始化、供电、背光和 RGB565 位图绘制
- `box2_motion_*`：SC7A20 初始化和三轴采样
- `box2_storage_*`：TF 卡挂载、容量读取和卸载

## 传输协议

协议版本 4 将每张 320×240 基线 JPEG 图片拆成多个 UDP 数据报。每个数据报包含 24 字节头部和最多 1400 字节 JPEG 数据，以尽量避免超过常见以太网 MTU。

主机先在 UDP 5001 广播发现请求，并向电脑本地 `/24` 网段发送定向广播；找到设备后，通过 `B2DS`/`B2DA` 版本握手建立 UDP 5000 串流会话。ESP32 只解码完整重组的 JPEG 帧。缺少任意分片时，该帧会在更新序列到来后被丢弃，因此丢包表现为跳帧，而不是残缺画面或不断增加的重传延迟。

硬件引脚和基础驱动定义参考 [xiaozhi-esp32 PR #954](https://github.com/78/xiaozhi-esp32/pull/954/changes)。
