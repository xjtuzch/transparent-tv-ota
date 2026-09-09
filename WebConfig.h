/*
  ============================================================
  WebConfig.h —— 透明小电视 网页配网模块
  ============================================================
  功能:
    1. 优先使用 NVS 中已保存的 WiFi 配置自动连接
    2. 失败时用主程序里的硬编码账号密码做保底连接
    3. 都连不上时开启配置热点 TransparentTV-XXXX(无密码),
       手机/电脑连上后浏览器任意网址都会跳转到配网页(Captive Portal)
    4. 配网页里选择/输入 WiFi 账号密码, 保存后自动连接并重启

  使用方法(主程序中):
    #include "WebConfig.h"
    // WiFi 连接(阻塞直到连上; 连不上时自动进入配网模式)
    WebConfig::begin(ssid, password);   // ssid/password 为硬编码保底

  小技巧:
    上电/复位时按住开发板 BOOT 键, 会清除已保存的 WiFi 并直接进入配网,
    方便换路由器后重新配置。
  ============================================================
*/

#ifndef WEBCONFIG_H
#define WEBCONFIG_H

#include <FS.h>            // 必须先于 TFT_eSPI 包含(与 OTAUpdate.h 同理)
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>     // 简易 Captive Portal: 任意网址 -> 配网页
#include <Preferences.h>   // NVS 存储

extern TFT_eSPI tft;       // 主程序定义的全局屏幕对象

namespace WebConfig {

const char* const AP_PREFIX   = "TransparentTV";
const unsigned long CONNECT_TIMEOUT_MS = 10000;   // 每次连接尝试的最长等待

WebServer  server(80);
DNSServer  dns;
Preferences prefs;

String lastNotice;         // 配网页上显示的提示(如"连接失败, 请重试")
String saveSsid, savePass; // /save 提交的临时内容

// ---------------- 小工具 ----------------
String esc(const String& s) {
  String r = s;
  r.replace("&", "&amp;");
  r.replace("<", "&lt;");
  r.replace(">", "&gt;");
  r.replace("\"", "&quot;");
  return r;
}

void drawCenter(const char* line1, const char* line2, const char* line3) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  if (line1) { tft.setCursor(5, 20, 1); tft.println(line1); }
  if (line2) { tft.setCursor(5, 55, 1); tft.println(line2); }
  if (line3) { tft.setCursor(5, 90, 1); tft.println(line3); }
}

// ---------------- NVS 存取 ----------------
void eraseSaved() {
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();
}

bool loadSaved(String& ssid, String& pass) {
  prefs.begin("wifi", true);
  bool ok = prefs.isKey("ssid") && prefs.isKey("pass");
  if (ok) {
    ssid = prefs.getString("ssid", "");
    pass = prefs.getString("pass", "");
  }
  prefs.end();
  return ok;
}

void saveCreds(const String& ssid, const String& pass) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

// ---------------- 连接辅助 ----------------
// 轮询等待 WiFi 连上, 超时返回 false (不阻塞太久)
bool waitConnected(unsigned long timeoutMs) {
  unsigned long t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(200);
  }
  return (WiFi.status() == WL_CONNECTED);
}

bool connectSTA(const String& ssid, const String& pass) {
  WiFi.begin(ssid.c_str(), pass.c_str());
  return waitConnected(CONNECT_TIMEOUT_MS);
}

// ---------------- 配网页 ----------------
String buildPage() {
  String html =
    "<!DOCTYPE html><html lang='zh'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>透明小电视 - WiFi 配网</title><style>"
    "body{background:#0a0f14;color:#e8eef2;font-family:sans-serif;"
    "max-width:420px;margin:0 auto;padding:2em 1em;text-align:center}"
    "input{width:92%;padding:.7em;margin:.4em 0;font-size:1em;"
    "background:#101820;color:#fff;border:1px solid #2a3a46;border-radius:8px}"
    ".pwdrow{display:flex;align-items:center;justify-content:center;gap:.4em}"
    ".pwdrow input{width:68%}"
    "#pwdBtn{width:24%;margin:.4em 0;padding:.7em .2em;font-size:1em;color:#111;"
    "background:#fff;border:0;border-radius:8px;cursor:pointer}"
    "button{width:92%;padding:.8em;margin:1em 0;font-size:1em;color:#fff;"
    "background:#1f6feb;border:0;border-radius:8px}"
    ".tip{color:#8fa3b0;font-size:.85em}"
    ".err{color:#ffc9c9;background:#3a1515;border-radius:8px;padding:.6em;margin:.5em 0}"
    "</style></head><body>"
    "<h2>透明小电视</h2><h3>WiFi 配网</h3>";

  if (lastNotice.length() > 0) {
    html += "<div class='err'>" + esc(lastNotice) + "</div>";
  }

  html +=
    "<form method='POST' action='/save'>"
    "<input list='nets' name='ssid' placeholder='WiFi 名称(仅支持 2.4GHz)' required>"
    "<datalist id='nets'>";

  // 扫描附近 WiFi(下拉建议, 也可手动输入)
  int n = WiFi.scanNetworks();
  if (n > 0) {
    for (int i = 0; i < n && i < 20; i++) {
      html += "<option value='" + esc(WiFi.SSID(i)) + "'>";
    }
  }
  WiFi.scanDelete();

  html +=
    "</datalist>"
    "<div class='pwdrow'>"
    "<input type='password' id='pwd' name='password' placeholder='WiFi 密码' required>"
    "<button type='button' id='pwdBtn' onclick='togglePwd()'>显示</button>"
    "</div>"
    "<button type='submit'>连接</button>"
    "</form>"
    "<p class='tip'>仅支持 2.4GHz WiFi。<br>连接成功后热点会自动关闭。</p>"
    "<script>"
    "function togglePwd(){var i=document.getElementById('pwd');"
    "var b=document.getElementById('pwdBtn');"
    "if(i.type==='password'){i.type='text';b.textContent='隐藏';}"
    "else{i.type='password';b.textContent='显示';}}"
    "</script>"
    "</body></html>";
  return html;
}

void handleRoot() {
  server.send(200, "text/html", buildPage());
}

void handleSave() {
  saveSsid   = server.arg("ssid");
  savePass   = server.arg("password");
  saveSsid.trim();
  if (saveSsid.length() == 0) {
    lastNotice = "WiFi 名称不能为空, 请重新输入。";
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
    return;
  }
  saveCreds(saveSsid, savePass);
  lastNotice = "";

  // 先回一页"正在连接", 浏览器会停留在这里; 连接成功后直接重启
  String page =
    "<!DOCTYPE html><html lang='zh'><head><meta charset='utf-8'>"
    "<meta http-equiv='refresh' content='18;url=/'>"
    "<title>正在连接...</title></head>"
    "<body style='background:#0a0f14;color:#e8eef2;text-align:center;"
    "padding:3em 1em;font-family:sans-serif'>"
    "<h2>正在连接</h2><h3>" + esc(saveSsid) + "</h3>"
    "<p>如果长时间停留在此页, 请重新打开 192.168.4.1 重试。</p>"
    "</body></html>";
  server.send(200, "text/html", page);

  // 保持热点在线(AP+STA 模式), 尝试连接路由器
  WiFi.mode(WIFI_AP_STA);
  bool ok = connectSTA(saveSsid, savePass);
  if (ok) {
    drawCenter("Connected!", "Rebooting...", NULL);
    delay(500);
    ESP.restart();          // 重启后走"已保存配置"自动连接流程
  } else {
    lastNotice = "连接失败, 请检查密码后重试。";
  }
}

// ---------------- 配置模式主循环 ----------------
// 热点界面提示(全英文, 手机/电脑靠近屏幕也能看懂)
void drawPortalScreen(const String& apName) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(1);

  // 第 1 行: 为什么进入配网模式
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(5, 10, 1);
  tft.println("Connection Failed");

  // 第 2~3 行: 请连接热点 + 热点名称
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(5, 28, 1);
  tft.println("Please Connect To");
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(5, 46, 1);
  tft.println(apName);

  // 第 4~5 行: 打开浏览器输入地址
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(5, 70, 1);
  tft.println("Then");
  tft.setCursor(5, 88, 1);
  tft.println("Open Browser & Visit");
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(5, 106, 1);
  tft.println("192.168.4.1");
}

// 阻塞运行: 开热点 + Captive Portal, 直到配置成功并重启
void runPortal() {
  String mac = WiFi.macAddress();
  String suffix = mac.substring(12, 14) + mac.substring(15, 17);  // 取 MAC 后两字节做后缀
  String apName = String(AP_PREFIX) + "-" + suffix;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName.c_str());          // 无密码热点
  IPAddress apIP = WiFi.softAPIP();

  drawPortalScreen(apName);             // 屏幕: 失败提示 + 热点名 + 配置地址
  Serial.println("热点: " + apName);
  Serial.println("配网页: http://192.168.4.1");

  dns.start(53, "*", apIP);             // Captive Portal: 任意域名 -> 配网页
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();

  for (;;) {                            // 永不退出, 直到 handleSave 里重启
    dns.processNextRequest();
    server.handleClient();
    delay(10);
  }
}

// ---------------- 对外入口 ----------------
// fbSsid / fbPass: 主程序里的硬编码保底账号(可为 NULL)
bool begin(const char* fbSsid, const char* fbPass) {
  pinMode(0, INPUT_PULLUP);             // BOOT 键按下 -> 清除配置并强制进配网
  bool forcePortal = (digitalRead(0) == LOW);
  if (forcePortal) {
    Serial.println("已按住 BOOT: 清除保存的 WiFi, 进入配网模式");
    eraseSaved();
  }

  // 1) 优先使用 NVS 保存的配置
  if (!forcePortal) {
    String s, p;
    if (loadSaved(s, p)) {
      drawCenter("Connecting...", s.c_str(), NULL);
      WiFi.mode(WIFI_STA);
      if (connectSTA(s, p)) return true;
    }
  }

  // 2) 硬编码账号密码保底
  if (!forcePortal && fbSsid != NULL && strlen(fbSsid) > 0) {
    drawCenter("Connecting...", fbSsid, NULL);
    WiFi.mode(WIFI_STA);
    if (connectSTA(fbSsid, fbPass == NULL ? "" : fbPass)) return true;
  }

  // 3) 都失败 -> 网页配网模式(阻塞直到配置完成重启)
  runPortal();
  return false;   // 不会执行到这里
}

}  // namespace WebConfig

#endif  // WEBCONFIG_H
