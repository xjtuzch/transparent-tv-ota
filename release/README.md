# 远程 OTA 发布指南(GitHub Releases)

设备固件内置 HTTPS + GitHub 根证书, 自动从仓库的 `release/manifest.json` 检查新版本,
新固件本身作为 **GitHub Release 附件** 下载。

```
raw.githubusercontent.com/.../main/release/manifest.json   ← 设备每次检查这里
        │  { build, url, md5 }
        ▼
github.com/.../releases/download/v31/TransparentScreen.ino.bin   ← 新固件
```

## 目录结构

- `manifest.json` —— 设备拉取的版本清单(每次发布后更新并 push)
- `update-manifest.ps1` —— 自动算 MD5 并生成 manifest 的小工具
- `certs/DigiCertGlobalRootCA.pem` —— 内置到固件的 GitHub 根证书(有效期至 2031-11)

## 重要前提

1. **仓库必须是 Public**。GitHub 的 raw 文件与 Release 下载都不允许匿名访问,
   私有仓库设备无法免登录下载。
2. 代码里硬编码了 WiFi 账号密码, 仓库公开前请先改掉
   (见 TransparentScreen.ino 顶部 `ssid/password`)。
3. 首次把设备从局域网 HTTP 模式切到 GitHub 模式时, 记得改 OTAUpdate.h 里的
   `OTA_MANIFEST_URL` 为你的仓库地址, 再编译烧录。

## 每次发布新固件的操作步骤

1. **改版本**: `OTAUpdate.h` 里把 `FW_VERSION` / `FW_BUILD` 加 1(例如 30 → 31)。
2. **编译**: Arduino IDE 菜单 项目 → 导出已编译的二进制文件,
   得到 `TransparentScreen.ino.bin`(约 1.3MB)。
3. **建 Release**: 打开 GitHub 仓库 → Releases → Draft a new release:
   - Tag: `v31`(与 manifest 的 url 保持一致)
   - 标题随意(如 "v31 新功能")
   - 附件: 上传 `TransparentScreen.ino.bin`
   - Publish release
4. **更新 manifest**(在电脑本地 release 目录):
   ```powershell
   .\update-manifest.ps1 -Build 31 -Version 31 `
     -BinPath .\TransparentScreen.ino.bin `
     -AssetUrl "https://github.com/你的用户名/你的仓库/releases/download/v31/TransparentScreen.ino.bin"
   ```
   或手动编辑 `manifest.json`(md5 用 `Get-FileHash -Algorithm MD5` 算小写)。
5. **push manifest**:
   ```bash
   git add release/manifest.json
   git commit -m "manifest: build 31"
   git push
   ```
6. 设备会在开机 30 秒后 / 每 6 小时 / 网页点"检查远程更新"时发现 build 31,
   自动下载刷写。新固件稳定运行 90 秒后自动确认; 连续 2 次启动失败会自动回滚旧版。

## 设备端相关配置(OTAUpdate.h)

```cpp
#define OTA_REMOTE_USE_HTTPS 1
#define OTA_MANIFEST_URL "https://raw.githubusercontent.com/你的用户名/你的仓库/main/release/manifest.json"
```

- 想切回局域网 HTTP 调试: 把 `OTA_REMOTE_USE_HTTPS` 改成 0,
  manifest url 指向你电脑上的 HTTP 服务器即可, 代码其余部分不用动。
- 证书到期前(2031-11)重新下载 `certs/DigiCertGlobalRootCA.pem` 并更新固件里的 `OTA_ROOT_CA`。

