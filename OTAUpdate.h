/*
  ============================================================
  OTAUpdate.h —— 透明小电视 局域网 OTA 升级模块
  ============================================================
  独立封装:
    1. ArduinoOTA   —— Arduino IDE 里的网络端口无线上传
    2. WebServer    —— 浏览器打开 http://transparent-tv.local 网页上传
  升级时会把小电视屏幕切换成进度画面, 完成后自动重启。

  使用方法:
    1. 主程序定义全局屏幕对象:  TFT_eSPI tft = TFT_eSPI();
    2. WiFi 连接成功后调用一次:  OTA::begin();
    3. 主循环开头判断暂停绘图:  if (OTA::isActive()) { delay(10); return; }

  OTA 处理运行在 core 0 的独立任务里,
  主循环中的阻塞延时不会影响上传(上传失败可回滚, 不会变砖)。
  ============================================================
*/

#ifndef OTAUPDATE_H
#define OTAUPDATE_H

// FS 必须先于 TFT_eSPI 包含: 否则 SMOOTH_FONT 会定义 FS_NO_GLOBALS,
// 导致 WebServer.h 里未限定的 FS 编不过 (ESP32 core 3.x 已知问题)
#include <FS.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// 主程序中定义的全局屏幕对象(由主程序负责创建)
extern TFT_eSPI tft;

namespace OTA {

/* ==================== 远程固件更新配置 ====================
 * 每次发布新固件都要做两件事:
 *   1) 把下面的 FW_BUILD 加 1(并同步改 FW_VERSION)
 *   2) 编译后用 TransparentScreen.ino.bin 更新远程服务器的 manifest.json
 */
#define FW_VERSION "30"                    // 当前固件版本号(只做提示/HTTP 头)
#define FW_BUILD   30                      // 构建号: 与 manifest 的 build 比较大小

#define OTA_REMOTE_USE_HTTPS 1             // 1 = HTTPS(GitHub Releases), 0 = HTTP(局域网调试)
#define OTA_MANIFEST_URL "https://raw.githubusercontent.com/xjtuzch/transparent-tv-ota/main/release/manifest.json"

#define OTA_CHECK_INTERVAL_MS   (6UL*3600UL*1000UL)   // 自动检查间隔: 6 小时
#define OTA_BOOT_CHECK_DELAY_MS (30UL*1000UL)         // 开机后延迟 30 秒再检查
#define OTA_BOOT_OK_MS          (90UL*1000UL)         // 新固件稳定运行 90 秒后确认"有效"
#define OTA_MAX_BOOT_TRIES      2                     // 连续 2 次启动失败 -> 自动回滚旧固件
#define OTA_AUTO_CHECK_ENABLED  1                     // 1 = 开机/定时自动检查; 0 = 只手动检查
#define OTA_REMOTE_TASK_STACK   24576                 // 远程检查独立任务栈(HTTPS+JSON+Update 需要较大栈)

#if OTA_REMOTE_USE_HTTPS
#include <WiFiClientSecure.h>
// GitHub 使用的 DigiCert Global Root CA(有效期至 2031-11)
// 由 https://cacerts.digicert.com/DigiCertGlobalRootCA.crt.pem 下载
const char OTA_ROOT_CA[] =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIDrzCCApegAwIBAgIQCDvgVpBCRrGhdWrJWZHHSjANBgkqhkiG9w0BAQUFADBh\n"
  "MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
  "d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBD\n"
  "QTAeFw0wNjExMTAwMDAwMDBaFw0zMTExMTAwMDAwMDBaMGExCzAJBgNVBAYTAlVT\n"
  "MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
  "b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IENBMIIBIjANBgkqhkiG\n"
  "9w0BAQEFAAOCAQ8AMIIBCgKCAQEA4jvhEXLeqKTTo1eqUKKPC3eQyaKl7hLOllsB\n"
  "CSDMAZOnTjC3U/dDxGkAV53ijSLdhwZAAIEJzs4bg7/fzTtxRuLWZscFs3YnFo97\n"
  "nh6Vfe63SKMI2tavegw5BmV/Sl0fvBf4q77uKNd0f3p4mVmFaG5cIzJLv07A6Fpt\n"
  "43C/dxC//AH2hdmoRBBYMql1GNXRor5H4idq9Joz+EkYIvUX7Q6hL+hqkpMfT7P\n"
  "T19sdl6gSzeRntwi5m3OFBqOasv+zbMUZBfHWymeMr/y7vrTC0LUq7dBMtoM1O/4\n"
  "gdW7jVg/tRvoSSiicNoxBN33shbyTApOB6jtSj1etX+jkMOvJwIDAQABo2MwYTAO\n"
  "BgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4EFgQUA95QNVbR\n"
  "TLtm8KPiGxvDl7I90VUwHwYDVR0jBBgwFoAUA95QNVbRTLtm8KPiGxvDl7I90VUw\n"
  "DQYJKoZIhvcNAQEFBQADggEBAMucN6pIExIK+t1EnE9SsPTfrgT1eXkIoyQY/Esr\n"
  "hMAtudXH/vTBH1jLuG2cenTnmCmrEbXjcKChzUyImZOMkXDiqw8cvpOp/2PV5Adg\n"
  "06O/nVsJ8dWO41P0jmP6P6fbtGbfYmbW0W5BjfIttep3Sp+dWOIrWcBAI+0tKIJF\n"
  "PnlUkiaY4IBIqDfv8NZ5YBberOgOzW6sRBc4L0na4UU+Krk2U886UAb3LujEV0ls\n"
  "YSEY1QSteDwsOoBrp+uvFRTp2InBuThs4pFsiv9kuXclVzDAGySj4dzp30d8tbQk\n"
  "CAUw7C29C79Fv1C5qfPrmAESrciIxpg0X40KPMbp1ZWVbd4=\n"
  "-----END CERTIFICATE-----\n";
#endif

// ---------------- 配置(按需修改) ----------------
const char otaHostname[] = "transparent-tv";  // 设备名, 浏览器用 http://transparent-tv.local

WebServer      server(80);       // 网页升级服务器
volatile bool  active = false;   // 升级期间暂停主界面绘制
size_t         uploadTotal = 0;  // 网页上传固件总大小 (从 Content-Length 读取)

// ---- 远程拉取固件状态 ----
volatile bool  remoteCheckRequested = false;  // 网页"/check"触发一次手动检查
unsigned long  nextRemoteCheckMs = 0;         // 下次自动检查时间
bool           remotePendingThisBoot = false; // 本固件是远程升级来的, 等确认
unsigned long  remoteBootMs = 0;              // 本次开机时刻(用于 90s 确认)
volatile bool  remoteJobRequested = false;    // 待执行的远程检查(网页按钮置位)
bool           remoteWorkerRunning = false;   // 远程检查任务是否在跑

// ---------------- 网页升级页 ----------------
const char indexPage[] =
"<!DOCTYPE html><html lang='zh'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>透明小电视 OTA</title><style>"
"body{background:#111;color:#eee;font-family:'Microsoft YaHei',sans-serif;text-align:center;padding:2em 1em}"
"h1{font-size:1.4em}.tip{color:#aaa}form{background:#1c1c1e;border:1px solid #333;"
"border-radius:12px;max-width:420px;margin:1.5em auto;padding:2em}"
"input[type=file]{display:block;margin:1em auto;color:#ddd}"
"button{background:#22c55e;color:#fff;border:0;border-radius:8px;padding:.7em 1.6em;font-size:1em;cursor:pointer}"
"button:hover{background:#16a34a}"
".btn2{display:block;width:92%;margin:.8em auto;padding:.75em;text-align:center;"
"color:#fff;background:#1f6feb;border-radius:8px;text-decoration:none;font-size:1em;cursor:pointer}"
".btn2:hover{background:#1958c4}"
"code{background:#333;padding:.15em .5em;border-radius:4px}"
"</style></head><body>"
"<h1>透明小电视 · 固件升级</h1>"
"<p class='tip'>选择编译好的 <code>.bin</code> 文件<br>(Arduino IDE 菜单: 项目 → 导出已编译的二进制文件)</p>"
"<form method='POST' action='/update' enctype='multipart/form-data'>"
"<input type='file' name='firmware' accept='.bin'>"
"<button type='submit'>开始升级</button></form>"
"<a class='btn2' href='/check'>检查远程更新</a>"
"<p class='tip'>升级期间请保持供电, 完成后设备会自动重启</p>"
"</body></html>";

// ---------------- 屏幕状态提示 ----------------
void showStart() {
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(30, 20, 1);
  tft.setTextSize(2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.println("OTA");
  tft.setCursor(5, 50, 1);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.println("Updating...");
  tft.drawRoundRect(10, 80, 108, 14, 4, TFT_WHITE);   // 进度条外框
}

void showProgress(unsigned int done, unsigned int total) {
  if (total == 0) return;
  int w = (int)((108L * done) / total);
  if (w > 104) w = 104;
  tft.fillRect(12, 82, w, 10, TFT_GREEN);
}

void showDone() {
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(10, 45, 1);
  tft.setTextSize(2);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.println("Success!");
  tft.setCursor(10, 80, 1);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.println("Rebooting...");
}

void showError(int code) {
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(10, 45, 1);
  tft.setTextSize(2);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.println("OTA Error");
  tft.setCursor(10, 80, 1);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.println("Code: " + String(code));
}

// ---------------- 远程拉取固件 (Remote OTA) ----------------
// NVS 记录: "pending" = 待确认的固件 build; "tries" = 该固件启动尝试次数
void savePendingBoot(uint32_t build) {
  Preferences p;
  p.begin("ota", false);
  p.putUInt("pending", build);
  p.putUChar("tries", 0);
  p.end();
}

void clearPendingBoot() {
  Preferences p;
  p.begin("ota", false);
  p.putUInt("pending", 0);
  p.putUChar("tries", 0);
  p.end();
}

// 每次开机调用: 如果上次是远程升级, 等待确认/必要时回滚
void bootCheckPending() {
  Preferences p;
  p.begin("ota", true);
  uint32_t pending = p.getUInt("pending", 0);
  uint8_t  tries   = p.getUChar("tries", 0);
  p.end();

  if (pending == 0) return;                 // 不是远程升级上来的
  if (pending != (uint32_t)FW_BUILD) {      // 固件被其他方式刷掉了, 清理残留
    clearPendingBoot();
    return;
  }

  tries++;
  p.begin("ota", false);
  p.putUChar("tries", tries);
  p.end();

  if (tries >= OTA_MAX_BOOT_TRIES) {
    Serial.println("[RemoteOTA] New firmware failed to boot twice, rollback...");
    clearPendingBoot();
    if (Update.rollBack()) {
      delay(500);
      ESP.restart();                        // 重启进旧分区
    }
    return;                                 // rollBack 失败则继续跑当前固件
  }

  Serial.println("[RemoteOTA] Pending build " + String(pending) +
                 ", will confirm after " + String(OTA_BOOT_OK_MS / 1000) + "s");
  remotePendingThisBoot = true;
  remoteBootMs = millis();
}

// 新固件稳定运行一段时间后, 把"待确认"清掉(之后不再可能被回滚)
void confirmBootOk() {
  if (!remotePendingThisBoot) return;
  if (millis() - remoteBootMs < OTA_BOOT_OK_MS) return;
  clearPendingBoot();
  remotePendingThisBoot = false;
  Serial.println("[RemoteOTA] New firmware confirmed OK");
}

// 拉取 manifest(HTTP/HTTPS 都走这里, 用宏切换)
bool fetchManifest(const String& url, String& out) {
#if OTA_REMOTE_USE_HTTPS
  // 注意: client 必须声明在 HTTPClient 之前!
  // HTTPClient 内部持有指向 client 的裸指针, 析构时还会调用 client->stop()。
  // 若 client 声明在后面, 离开作用域时 client 先被析构, HTTPClient 再对已析构
  // 对象调用 stop() -> 连接失败(HTTP -1)后 100% 触发 Core 0 InstrFetchProhibited。
  WiFiClientSecure client;
  client.setCACert(OTA_ROOT_CA);
#else
  WiFiClient client;
#endif
  HTTPClient http;   // 必须后声明: 保证 http 先析构(此时 client 仍存活)
  if (!http.begin(client, url)) {
    Serial.println("[RemoteOTA] Manifest connect failed");
    http.end();
    return false;
  }
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("ESP32-TransparentTV");
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.println("[RemoteOTA] Manifest HTTP code: " + String(code));
    http.end();
    return false;
  }
  out = http.getString();
  http.end();
  return true;
}

// 远程检查 + 下载新固件(阻塞, 运行在独立的高栈任务里)
void runRemoteCheck() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[RemoteOTA] Skip: WiFi not connected");
    return;
  }
  Serial.println("[RemoteOTA] Checking manifest ... (free heap "
                 + String(ESP.getFreeHeap()) + ")");

  String manifest;
  Serial.println("[RemoteOTA] Fetching manifest ...");
  if (!fetchManifest(OTA_MANIFEST_URL, manifest)) return;

  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, manifest)) {
    Serial.println("[RemoteOTA] Manifest JSON parse failed");
    return;
  }
  Serial.println("[RemoteOTA] Manifest OK");
  int  build = doc["build"] | -1;
  const char* url = doc["url"] | "";
  const char* md5 = doc["md5"] | "";
  const char* ver = doc["version"] | "";

  if (build <= 0 || strlen(url) == 0) {
    Serial.println("[RemoteOTA] Manifest missing build/url");
    return;
  }
  if (build <= FW_BUILD) {
    Serial.println("[RemoteOTA] Up to date (build " + String(FW_BUILD) + ")");
    return;
  }

  Serial.println("[RemoteOTA] New build " + String(build) + " found, begin download ...");
  active = true;                            // 暂停主界面
  showStart();
  tft.setCursor(5, 6, 1);
  tft.setTextSize(1);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.println("Remote FW v" + String(ver));

  HTTPUpdate upd(10000, &Update);           // 空闲 10s 视为失败
  upd.rebootOnUpdate(false);                // 成功后由我们保存状态再重启
  upd.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (strlen(md5) > 0) upd.setMD5sum(md5);
  upd.onProgress([](int done, int total) { showProgress((unsigned)done, (unsigned)total); });
  upd.onError([](int err) { Serial.println("[RemoteOTA] httpUpdate error: " + String(err)); });

#if OTA_REMOTE_USE_HTTPS
  WiFiClientSecure client;
  client.setCACert(OTA_ROOT_CA);
#else
  WiFiClient client;
#endif

  // GitHub 下载链接会 302 跳转到 objects.githubusercontent.com,
  // HTTPUpdate 会在同一 secure client 上自动跟随; 加 User-Agent 更稳。
  t_httpUpdate_return ret = upd.update(
      client, String(url), String(FW_VERSION),
      [](HTTPClient* c) { c->setUserAgent("ESP32-TransparentTV"); });
  if (ret == HTTP_UPDATE_OK) {
    Serial.println("[RemoteOTA] Flash OK, rebooting into build " + String(build));
    savePendingBoot(build);                 // 新固件下一次开机用它做"确认/回滚"
    showDone();
    delay(1500);
    ESP.restart();
  } else if (ret == HTTP_UPDATE_NO_UPDATES) {
    Serial.println("[RemoteOTA] No updates");
    active = false;
    tft.fillScreen(TFT_BLACK);
  } else {
    Serial.println("[RemoteOTA] FAILED: " + String(upd.getLastError()) +
                   " " + upd.getLastErrorString());
    showError(upd.getLastError());
    delay(3000);
    active = false;
    tft.fillScreen(TFT_BLACK);
  }
}

// 远程检查的独立任务入口:
// runRemoteCheck 内部要做 HTTP 下载 + JSON 解析 + HTTPUpdate,
// 需要的栈远大于 otaTask 的 8KB, 之前直接在里面调用导致
// Core 0 InstrFetchProhibited(栈溢出)并在每次开机 45s 后反复崩溃。
// 现在把它挪到 20KB 独立任务里运行。
void remoteCheckTask(void* parameter) {
  Serial.println("[RemoteOTA] Worker task begin (stack "
                 + String(OTA_REMOTE_TASK_STACK) + " bytes)");
  runRemoteCheck();
  Serial.println("[RemoteOTA] Worker task done");
  remoteWorkerRunning = false;
  vTaskDelete(NULL);
}

// 触发一次远程检查(若已有检查在跑则忽略)
void startRemoteCheck() {
  if (remoteWorkerRunning) {
    Serial.println("[RemoteOTA] Check already running, skip");
    return;
  }
  remoteWorkerRunning = true;               // 先预占, 防止重复创建任务
  BaseType_t ok = xTaskCreatePinnedToCore(remoteCheckTask, "otaRemoteCheck",
                                          OTA_REMOTE_TASK_STACK, NULL, 1, NULL, 0);
  if (ok != pdPASS) {
    remoteWorkerRunning = false;
    Serial.println("[RemoteOTA] Failed to create worker task (out of memory?)");
  }
}

// ---------------- 初始化 ArduinoOTA + 网页升级 ----------------
void initOTA() {
  ArduinoOTA.setHostname(otaHostname);

  ArduinoOTA.onStart([]() {
    active = true;              // 暂停主界面绘制, 避免和升级画面抢屏幕
    showStart();
    Serial.println("OTA update started");
  });
  ArduinoOTA.onEnd([]() {
    showDone();
    Serial.println("OTA update finished");
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    showProgress(done, total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.print("OTA Error: ");    // 不用 printf, 避免引入约22KB的printf支持代码
    Serial.println((int)error);
    showError((int)error);
    active = false;                 // 失败后恢复主界面
  });
  ArduinoOTA.begin();

  MDNS.addService("http", "tcp", 80);   // 让浏览器能通过 http://transparent-tv.local 打开升级页

  // 网页升级页
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", indexPage);
  });
  // 手动"检查远程更新"
  server.on("/check", HTTP_GET, []() {
    String page;
    if (active) {
      page = "<html><head><meta charset='utf-8'><meta http-equiv='refresh' content='5;url=/'>"
             "<title>忙碌中</title></head><body style='background:#111;color:#eee;text-align:center;"
             "padding:3em 1em;font-family:sans-serif'><h2>正在升级, 请稍后再试</h2>"
             "<p>5 秒后自动返回</p></body></html>";
    } else {
      remoteCheckRequested = true;
      page = "<html><head><meta charset='utf-8'><meta http-equiv='refresh' content='5;url=/'>"
             "<title>正在检查</title></head><body style='background:#111;color:#eee;text-align:center;"
             "padding:3em 1em;font-family:sans-serif'><h2>已开始检查远程更新</h2>"
             "<p>有新版本时请观察设备屏幕上的进度。</p>"
             "<p>5 秒后自动返回本页。</p></body></html>";
    }
    server.send(200, "text/html", page);
  });
  // 处理上传的 .bin 固件
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    if (active) {                       // 只有升级成功才重启
      server.send(200, "text/plain", "OK");
      delay(100);
      ESP.restart();                    // 成功后重启进入新固件
    } else {
      server.send(200, "text/plain", "FAIL");
    }
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      active = true;
      uploadTotal = server.header("Content-Length").toInt();
      showStart();
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (active) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
          Update.printError(Serial);
        }
        if (uploadTotal > 0) {
          showProgress(upload.totalSize, uploadTotal);   // 网页上传也显示进度
        }
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (active) {
        uploadTotal = 0;
        if (Update.end(true)) {
          showDone();
        } else {
          Update.printError(Serial);
          showError(99);
          active = false;
        }
      }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
      if (active) {
        Update.abort();
        active = false;
        uploadTotal = 0;
      }
    }
  });
  server.begin();

  // 收集 Content-Length 头, 用于网页上传时显示进度
  const char* headerKeys[] = {"Content-Length"};
  server.collectHeaders(headerKeys, 1);
}

// ---------------- 后台任务: 跑在 core 0, 主循环延时不会阻塞升级 ----------------
void task(void* parameter) {
  for (;;) {
    ArduinoOTA.handle();
    server.handleClient();

    // 新固件启动确认: 稳定运行 90s 后解除回滚等待
    confirmBootOk();

    // 远程固件检查: 手动按钮优先, 其次是定时(开机延迟 45s, 之后每 6 小时)
    // 注意: 这里只"发起"检查, 真正下载运行在独立的大栈任务里,
    // 避免在 otaTask 的 8KB 栈上执行 HTTP+Update 造成栈溢出崩溃。
    if (!active) {
      if (remoteCheckRequested) {
        remoteCheckRequested = false;
        nextRemoteCheckMs = millis() + OTA_CHECK_INTERVAL_MS;
        startRemoteCheck();
      } else if (OTA_AUTO_CHECK_ENABLED && WiFi.status() == WL_CONNECTED &&
                 (int32_t)(millis() - nextRemoteCheckMs) >= 0) {
        nextRemoteCheckMs = millis() + OTA_CHECK_INTERVAL_MS;
        startRemoteCheck();
      }
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// ---------------- 对外接口 ----------------
// WiFi 连接成功后调用一次
void begin() {
  bootCheckPending();                 // 远程升级后的开机确认/回滚检查
  initOTA();
  remoteBootMs = millis();            // 若本次是新固件, 从这里开始计 90s
  nextRemoteCheckMs = millis() + OTA_BOOT_CHECK_DELAY_MS;
  xTaskCreatePinnedToCore(task, "otaTask", 8192, NULL, 1, NULL, 0);
  WiFi.setSleep(false);               // 关闭 WiFi 省电休眠, 提升 OTA/网络稳定性
  Serial.println("OTA ready: http://" + String(otaHostname) + ".local");
  Serial.println("Firmware: v" + String(FW_VERSION) + " (build " + String(FW_BUILD) + ")");
  Serial.println("Remote manifest: " + String(OTA_MANIFEST_URL));
}

// 是否正在升级(主循环用它暂停界面绘制)
bool isActive() {
  return active;
}

}  // namespace OTA

#endif  // OTAUPDATE_H
