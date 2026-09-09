# TransparentScreen —— ESP32 透明小电视

ESP32-D0WD-V3(WROOM-32)+ ST7735 128x128 透明屏小电视。

## 主要功能

- 时间页(太空人动画)+ 天气页自动轮播
- Open-Meteo 天气 + IP 定位(国外可用, 默认 Toronto)
- Web 配网(Captive Portal)+ 局域网 OTA + GitHub Releases 远程 OTA
- 独立 Demo: 特效展示 / INMP441 FFT 频谱 / 番茄钟(见 EffectsDemo、FFTDemo、PomodoroDemo)

## 目录

- `TransparentScreen.ino` / `OTAUpdate.h` / `WebConfig.h` / `MyFont.h` —— 主固件
- `Pic/` —— 图片素材
- `release/` —— GitHub 远程升级的 manifest 与发布工具(用法见其 README)

## 发布新固件

详见 [release/README.md](release/README.md)。

## 安全提醒

源码内含本地 WiFi 凭据与 OTA 网页密码, 上传到 GitHub 前请改成占位符或用 Web 配网
NVS 保存; 远程升级用的仓库需保持 Public。

