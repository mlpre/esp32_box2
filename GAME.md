# 只因你太美

开机直接玩。**轻晃一下开发板，真人背身角色顶球入篮，每进一球加 2 分。**

没有菜单、握姿校准、倒计时、谱面或动作组合。球在空中时不重复发射，落入篮网后自动补球，再晃一次即可继续。

## 操作

- **轻晃设备**：顶球，任何握姿均可。
- **M**：同样触发顶球，按下即生效。L 也可以触发。
- **Q**：清零当前得分、重新玩。
- **R**：循环音量，55 → 75 → 静音 → 35。

声音只保留精华：**4.28 秒副歌口号循环**，去掉自我介绍、长伴奏和结束语；每五个进球播放一次 **1.20 秒“你干嘛”原声短句**。音频已经内置在 Flash，无需 TF 卡或 Wi-Fi。

## 人物与声音

角色使用原篮球梗的两帧真人背面透明抠图，转肩时切换姿势。背景录音及原声来自已有梗游戏资源，属于录音片段，并非 MIDI、声音克隆或经核实的完整录音室母带。

来源链接、原始文件名、版本和资源处理方式见 [素材来源](main/assets/SOURCES.md)。

## 构建与烧录

ESP-IDF 6.0.2 PowerShell：

```powershell
idf.py build
idf.py -p COM5 flash
```

默认构建背身顶球游戏。保留原硬件测试代码，可切换：

```powershell
idf.py -DBOX2_HARDWARE_TEST=ON reconfigure build
# 切回游戏
idf.py -DBOX2_HARDWARE_TEST=OFF reconfigure build
```

## 源码

- `main/game.c`：单动作触发、去重、抛物线、过篮计分、重新开始。
- `main/game_render.c`：灰色练习室、篮板、透明真人背面图片、球与篮网前后遮挡。
- `main/game_main.c`：50 Hz 体感采样、按键防抖、约 30 FPS 显示目标、独立音频流和 NVS。
- `main/game_selftest.c`：板上与桌面共用自检，覆盖轨迹、过篮、单次得分、防重复、重置、任意握姿检测。
- `main/assets/`：原始透明 PNG、PCM 音频、来源和许可证。

最高分保存在 `kun_basket` NVS 命名空间，与旧节奏游戏隔离。球回到待发位置后保存，不在飞行中写 Flash。

## 素材生成与桌面验证

PNG 的原始像素保持不变，生成 ARGB 数组：

```powershell
./tools/generate_game_sprites.ps1
./tools/generate_game_font.ps1
cmd.exe /c tools\preview_game.cmd
./tools/preview_to_png.ps1
```

桌面程序用同一套游戏和绘图代码执行自检，输出待发、顶肩、飞行、进篮四张图，合图为 `build/game_preview.png`。预览依赖本机 VS 2022 Build Tools，字体打包使用 Windows 微软雅黑/Consolas。

音频已随源码提交。重新转码可以使用 `tools/import_game_audio.ps1 -Background <原背景音路径> -Voice <原声路径>`，其增益及淡出针对当前素材设置。

## 本次验证

2026-09-08：ESP-IDF 6.0.2 编译成功，应用为 669,648 字节。COM5 烧录并通过哈希校验。桌面和板上均打印 `BUMP SELFTEST PASS`；串口确认 `BACK-BUMP GAME READY | sensor=1 audio=1 music=4.28s voice=1.20s`，观察期内持续正常运行，并记录到实际体感顶球和两次进篮加分。启动记录见本机 `build/game_boot.log`。实体屏幕观感、玩家手感和扬声器听感仍需实际体验。
