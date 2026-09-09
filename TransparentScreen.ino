#include <FS.h>         // 必须先于 TFT_eSPI 包含: 否则 SMOOTH_FONT 会定义 FS_NO_GLOBALS, 导致 WebServer.h 编不过
#include <TFT_eSPI.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>    //wifi库
#include <NTPClient.h>    //NTP库
#include <ArduinoJson.h>  //Json库
#include <HTTPClient.h>  //HTTP库
#include <WiFiUdp.h>
#include "Arduino.h"
#include "OTAUpdate.h"   // OTA 升级功能独立模块 (ArduinoOTA + 网页升级)
#include "WebConfig.h"   // 网页配网模块 (NVS保存配置 -> 硬编码保底 -> 配网热点)
#include "MyFont.h"  // 自制字体模板库
#include "./Pic/Astronaut/As.h"
#include "./Pic/weather/Weather.h"

/*********注意填写自己Wifi的账号密码**********/
const char *ssid     = "OPPO Find X8s+";  //Wifi账号
const char *password = "wezr9974"; //Wifi密码

/* 国外可用的网络服务(全部走 HTTP 80, 无需 API Key)
 * 1. ip-api.com      : 根据公网 IP 自动定位城市 + IANA 时区
 * 2. open-meteo      : 城市名 -> 经纬度(Geocoding) 与 天气预报
 * 3. time.cloudflare.com : NTP 服务器, 全球可达
 */
const char* GEO_HOST = "geocoding-api.open-meteo.com";  // 城市名 -> 经纬度
const char* WEA_HOST = "api.open-meteo.com";            // 天气预报
const char* IP_HOST  = "ip-api.com";                    // IP -> 城市/时区

String now_high_tem="--",now_low_tem="--",now_rainfall="--",now_wind_direction="北",now_wind_scale="",now_hum="--"; //天气页数据
String now_wea = "晴";       // 天气汉字(最多 2 字: 晴/多云/阴/雾/雨/雪/雷雨)
String weekDays[7]={"周日", "周一", "周二","周三", "周四", "周五", "周六"};

/* ---- 定位结果(默认 Toronto; 上电 IP 定位成功后会覆盖) ---- */
String now_city   = "Toronto";          // 英文城市名
String now_city_zh= "";                 // Open-Meteo Geocoding 返回的中文城市名(可为空)
String now_region = "Ontario";          // 省/州(城市名太长时备用)
String now_country= "CA";               // 国家代码
String now_tz     = "America/Toronto";  // IANA 时区
float  locLat = 43.6532f;               // Toronto 默认坐标
float  locLon = -79.3832f;

/* 英文城市 -> 中文(显示时只取"字库里都有的汉字", 缺字自动回退英文) */
struct CnCity { const char* en; const char* zh; };
const CnCity caCities[] = {
  {"Toronto",    "多伦多"},
  {"Vancouver",  "温哥华"},
  {"Montreal",   "蒙特利尔"},
  {"Ottawa",     "渥太华"},
  {"Calgary",    "卡尔加里"},
  {"Edmonton",   "埃德蒙顿"},
  {"Winnipeg",   "温尼伯"},
  {"Quebec",     "魁北克"},
  {"Halifax",    "哈利法克斯"}
};

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "time.cloudflare.com"); // NTP服务器(Cloudflare, 全球可用)
TFT_eSPI tft = TFT_eSPI();  //设定屏幕
int i = 0;    // 太空人动画帧号
int ph = 0;   // 天气大图标索引
long g_tzOffset = 0;   // 当前写入 NTPClient 的时区秒偏移(NTPClient 库没有 getter, 自己记录)
unsigned long lastTzSyncMs = 0;
const unsigned long TZ_SYNC_MS = 60000;   // 每 60s 重新校正一次 DST/时区

/**********页面轮播与动画调度(millis, 无阻塞)**********/
const unsigned long TIME_PAGE_MS    = 5000;   // 时间页显示时长
const unsigned long WEATHER_PAGE_MS = 5000;   // 天气页显示时长
const unsigned long ASTRO_FRAME_MS  = 120;    // 太空人动画帧间隔
const unsigned long WEATHER_REFRESH_MS = 600000UL; // 天气每 10 分钟刷新一次
unsigned long pageStartMs = 0;                // 当前页面开始时刻
unsigned long astroTickMs = 0;                // 上一次太空人换帧时刻
unsigned long lastWeatherMs = 0;              // 上一次成功发起天气请求的时刻
bool showWeatherNow = false;                  // true = 正在显示天气页
String dispTime, dispDate;                    // 时间页显示 "HH:MM" 与 "M/D"
int    dispYear;
String dispWeek;

/*************Connect Wifi********************/
// 连接优先级: ① NVS 已保存配置 ② 文件顶部硬编码保底 ③ 网页配网热点
void get_wifi()
{
    WebConfig::begin(ssid, password);        // 阻塞直到连上(连不上时自动开配网热点)

    Serial.println("WiFi connected");        //连接成功
    Serial.print("IP address: ");            //打印IP地址
    Serial.println(WiFi.localIP());
    tft.fillScreen(TFT_BLACK);
    //tft.pushImage(14, 65, 100, 20, ConnectWifi[5]);//调用图片数据
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(20, 30, 1);                //设置文字开始坐标(20,30)及1号字体
    tft.setTextSize(1);
    tft.println("WiFi Connected!");
    delay(200);
}
/*************** HTTP 小工具 ***************/
// 向 host 发送 GET 请求, 返回以 '{' 开头的 JSON 正文; 成功返回 true
bool httpGetBody(const char* host, const String& path, String& body, unsigned long timeoutMs)
{
  WiFiClient client;
  if (!client.connect(host, 80)) {
    Serial.println("HTTP connect failed: " + String(host));
    return false;
  }
  client.print(String("GET ") + path + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "User-Agent: ESP32-TransparentTV/1.0\r\n" +
               "Connection: close\r\n\r\n");

  unsigned long t0 = millis();
  String raw;
  while (millis() - t0 < timeoutMs) {
    while (client.available()) raw += (char)client.read();
    if (!client.connected() && !client.available()) break;
    delay(2);
  }
  client.stop();

  int p = raw.indexOf('{');
  if (p < 0) {
    Serial.println("HTTP no JSON body from: " + String(host));
    return false;
  }
  body = raw.substring(p);
  return true;
}

// 简单 URL 编码(城市名里可能有空格 / 撇号等)
String urlEncode(const String& s)
{
  const char* hexd = "0123456789ABCDEF";
  String r;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
      r += c;
    } else {
      r += '%';
      r += hexd[((unsigned char)c) >> 4];
      r += hexd[((unsigned char)c) & 0x0F];
    }
  }
  return r;
}

/*************** 字库与城市名显示 ***************/
// 检查一句话里的每个汉字是否都在 MyFont.h 字库里(缺字则不该显示, 避免空白)
bool fontHasAllGlyphs(const char* str)
{
  int len = (int)strlen(str);
  if (len == 0 || len % 3 != 0) return false;
  size_t n = sizeof(hanzi) / sizeof(hanzi[0]);
  for (int i = 0; i < len; i += 3) {
    bool found = false;
    for (size_t k = 0; k < n; k++) {
      if (hanzi[k].Index[0] == str[i] &&
          hanzi[k].Index[1] == str[i+1] &&
          hanzi[k].Index[2] == str[i+2]) { found = true; break; }
    }
    if (!found) return false;
  }
  return true;
}

char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

bool sameCityName(const String& a, const char* b)
{
  size_t n = strlen(b);
  if (a.length() != n) return false;
  for (size_t i = 0; i < n; i++) {
    if (lowerAscii(a.charAt((unsigned int)i)) != lowerAscii(b[i])) return false;
  }
  return true;
}

// 返回城市中文名; 只有该中文名的每个汉字都在字库里时才返回(否则回退英文)
String cityDisplayString()
{
  size_t n = sizeof(caCities) / sizeof(caCities[0]);
  for (size_t i = 0; i < n; i++) {
    if (sameCityName(now_city, caCities[i].en) && fontHasAllGlyphs(caCities[i].zh)) {
      return String(caCities[i].zh);
    }
  }
  // 国内定位时 Geocoding 返回的中文名(如 杭州/上海), 字库齐全才显示
  if (now_city_zh.length() > 0 && fontHasAllGlyphs(now_city_zh.c_str())) {
    return now_city_zh;
  }
  return "";
}

/*************** 风向 -> 中文(八字位) ***************/
String compassCn(int deg)
{
  static const char* dirs[] = {"北", "东北", "东", "东南", "南", "西南", "西", "西北"};
  int a = ((deg % 360) + 360) % 360;          // 0~359, 0 = 正北
  int idx = ((a + 22) % 360) / 45;            // 每 45° 一档, 从正北开始
  return String(dirs[idx]);
}

/*************** Open-Meteo WMO 天气代码 -> 汉字 + 图标 ***************/
// WMO code 参考: 0晴 1基本晴 2多云 3阴 45/48雾 51~57毛毛雨 61~67雨
// 71~77雪 80~82阵雨 85/86阵雪 95~99雷暴(含冰雹)
void wmoToDisplay(int code)
{
  now_wea = "晴"; ph = 0;
  if (code == 0 || code == 1) {
    now_wea = "晴"; ph = 0;
  } else if (code == 2) {
    now_wea = "多云"; ph = 1;
  } else if (code == 3) {
    now_wea = "阴"; ph = 2;
  } else if (code == 45 || code == 48) {
    now_wea = "雾"; ph = 2;          // 需要新汉字: 雾
  } else if (code >= 51 && code <= 57) {
    now_wea = "雨"; ph = 3;          // 毛毛雨
  } else if (code >= 61 && code <= 67) {
    now_wea = "雨";                  // 小雨/中雨/大雨/冻雨
    ph = (code == 65 || code == 67) ? 5 : (code == 63 ? 4 : 3);
  } else if (code >= 71 && code <= 77) {
    now_wea = "雪";
    ph = (code == 71 || code == 77) ? 6 : (code == 73 ? 7 : 8);
  } else if (code >= 80 && code <= 82) {
    now_wea = "雨"; ph = (code == 82) ? 5 : 4;   // 阵雨
  } else if (code >= 85 && code <= 86) {
    now_wea = "雪"; ph = (code == 85) ? 6 : 7;   // 阵雪
  } else if (code >= 95) {
    now_wea = "雷雨"; ph = 5;        // 需要新汉字: 雷
  }
}

void weatherUnavailable()
{
  now_high_tem = "--";
  now_low_tem  = "--";
  now_rainfall = "--";
  now_hum      = "--";
  now_wind_direction = "";
  now_wea = "";
  ph = 0;
}

/*************** IP 自动定位 -> 城市 / 时区 / 经纬度 ***************/
// 流程: ip-api 拿城市+时区 -> Open-Meteo Geocoding 用城市名查经纬度
//       (任一步失败就回退到顶部默认值 Toronto)
void detectLocationByIP()
{
  Serial.println("Locating by IP ...");
  now_city_zh = "";                       // 每次定位都重新取中文城市名
  String body;
  if (!httpGetBody(IP_HOST,
                   "/json/?fields=status,message,countryCode,regionName,city,lat,lon,timezone,query",
                   body, 6000)) {
    Serial.println("IP locate failed, use Toronto default");
    return;
  }

  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, body)) {
    Serial.println("IP locate JSON error, use Toronto default");
    return;
  }
  if (String((const char*)(doc["status"] | "fail")) != "success") {
    Serial.println("IP locate status fail, use Toronto default");
    return;
  }

  String cc  = doc["countryCode"] | "";
  String tz  = doc["timezone"] | "";
  String city = doc["city"] | "";
  String region = doc["regionName"] | "";
  float ipLat = doc["lat"] | -999.0f;
  float ipLon = doc["lon"] | -999.0f;
  bool haveIpCoord = (ipLat > -90.0f && ipLat < 90.0f && ipLon > -180.0f && ipLon < 180.0f);

  // 中国/加拿大都做自动定位: 国内显示国内天气, 加拿大显示当地天气;
  // 其他地区暂时回退 Toronto 默认值(时区表未覆盖, 避免时间错乱)
  if (cc != "CA" && cc != "CN") {
    Serial.println("IP outside CA/CN, keep Toronto default");
    return;
  }

  if (cc.length()   > 0) now_country = cc;
  if (tz.length()   > 0) now_tz = tz;
  if (city.length() > 0) {
    now_city = city;
  } else if (region.length() > 0) {
    now_city = region;      // 城市字段为空时退而求其次显示省/州
  } else {
    Serial.println("IP locate empty city, use Toronto default");
    return;
  }
  if (region.length() > 0) now_region = region;

  // 城市名 -> 经纬度(Open-Meteo Geocoding, 限定国家避免同名城市)
  bool gotCoord = false;
  String gpath = "/v1/search?name=" + urlEncode(now_city) +
                 "&count=1&language=zh&format=json";   // language=zh: 拿中文城市名
  if (now_country.length() > 0) gpath += "&countryCode=" + now_country;   // 官方参数是 countryCode
  String gbody;
  if (httpGetBody(GEO_HOST, gpath, gbody, 6000)) {
    StaticJsonDocument<1536> gdoc;
    if (!deserializeJson(gdoc, gbody)) {
      JsonObject r0 = gdoc["results"][0];
      float glat = r0["latitude"] | -999.0f;
      float glon = r0["longitude"] | -999.0f;
      if (glat > -90.0f && glat < 90.0f && glon > -180.0f && glon < 180.0f) {
        locLat = glat;
        locLon = glon;
        gotCoord = true;
      }
      // 保存返回的中文城市名(仅当含汉字时保留, 便于字库判断)
      String gn = r0["name"] | "";
      bool hasCn = false;
      for (unsigned int q = 0; q < gn.length(); q++) {
        if ((unsigned char)gn.charAt(q) >= 0x80) { hasCn = true; break; }
      }
      if (hasCn) now_city_zh = gn;
    }
  }
  if (!gotCoord && haveIpCoord) {   // Geocoding 找不到时用 IP 自身的坐标兜底
    locLat = ipLat;
    locLon = ipLon;
  }

  Serial.println("Location: " + now_city + " / " + now_city_zh + " / " + now_tz +
                 " / " + String(locLat, 4) + "," + String(locLon, 4));
}

/*************** 天气(Open-Meteo) ***************/
void get_weather()
{
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Weather: WiFi not connected");
    weatherUnavailable();
    return;
  }

  String path = "/v1/forecast?latitude=" + String(locLat, 4) +
                "&longitude=" + String(locLon, 4) +
                "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,wind_direction_10m" +
                "&daily=temperature_2m_max,temperature_2m_min,precipitation_sum,weather_code" +
                "&timezone=auto&forecast_days=1";

  String body;
  if (!httpGetBody(WEA_HOST, path, body, 7000)) {
    Serial.println("Weather request failed");
    weatherUnavailable();
    return;
  }

  StaticJsonDocument<2048> doc;
  if (deserializeJson(doc, body)) {
    Serial.println("Weather JSON parse failed");
    weatherUnavailable();
    return;
  }

  JsonObject cur = doc["current"];
  int curCode = cur["weather_code"] | -1;
  int dayCode = doc["daily"]["weather_code"][0] | curCode;
  wmoToDisplay(dayCode >= 0 ? dayCode : 0);       // 图标按白天预报取, 避免夜间误显示晴空

  float hiV = doc["daily"]["temperature_2m_max"][0] | -999.0f;
  float loV = doc["daily"]["temperature_2m_min"][0] | -999.0f;
  now_high_tem = (hiV > -990.0f) ? String((int)roundf(hiV)) : "--";
  now_low_tem  = (loV > -990.0f) ? String((int)roundf(loV)) : "--";

  int hum = cur["relative_humidity_2m"] | -1;
  now_hum = (hum >= 0) ? String(hum) : "--";

  float rain = doc["daily"]["precipitation_sum"][0] | -1.0f;
  if (rain < -0.5f) now_rainfall = "--";
  else if (rain < 10.0f) now_rainfall = String(rain, 1);
  else now_rainfall = String((int)(rain + 0.5f));

  int wdir = cur["wind_direction_10m"] | -1;
  now_wind_direction = (wdir >= 0) ? compassCn(wdir) : "北";
  float ws = cur["wind_speed_10m"] | -1.0f;
  now_wind_scale = (ws >= 0) ? String((int)(ws + 0.5f)) : "";

  Serial.println("Weather: " + now_city + " hi=" + now_high_tem +
                 " lo=" + now_low_tem + " hum=" + now_hum +
                 " rain=" + now_rainfall + " wind=" + now_wind_direction +
                 " text=" + now_wea + " icon=" + String(ph));
}

/******************* 公历换算与本地时区(加拿大 DST) *******************/
/*
 * 时间链路:
 *   NTP(time.cloudflare.com) 提供 UTC 秒数;
 *   NTPClient 的 getEpochTime() 会加上 setTimeOffset() 的秒偏移;
 *   这里先按 UTC 存储, 再用 IP 定位到的 IANA 时区名(如 America/Toronto)
 *   计算加拿大夏令时规则下的正确偏移, 每分钟校正一次, 跨 DST 自动切换。
 *
 *   换算用 Howard Hinnant 的 civil_from_days / days_from_civil 纯整数算法,
 *   不依赖 gmtime 等库函数, 结果确定可靠。
 */
int curYear = 1970, curMonth = 1, curDay = 1;   // 换算结果(本地年月日)

// 公历日期 -> 自 1970-01-01 的天数(与 epochToYmd 互逆)
long long daysFromCivil(int y, int m, int d)
{
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long long)era * 146097 + (long long)doe - 719468;
}

// 纯整数: epoch 秒 -> (年, 月, 日)
void epochToYmd(unsigned long epochSec, int& y, int& m, int& d)
{
    long long days = (long long)epochSec / 86400LL;
    long long z = days + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned long doe = (unsigned long)(z - era * 146097);
    unsigned long yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    long long yy = (long long)yoe + era * 400;
    unsigned long doy = doe - (365*yoe + yoe/4 - yoe/100);
    unsigned long mp  = (5*doy + 2)/153;
    unsigned long dd  = doy - (153*mp+2)/5 + 1;
    unsigned long mm  = (mp < 10) ? mp + 3 : mp - 9;
    if (mm <= 2) yy += 1;
    y = (int)yy;
    m = (int)mm;
    d = (int)dd;
}

void epochToDate(unsigned long epochSec)
{
    epochToYmd(epochSec, curYear, curMonth, curDay);
}

// 美加夏令时(2007 年后): 3 月第二个周日 02:00 ~ 11 月第一个周日 02:00
// stdSec/dstSec 为该时区标准/夏令时相对 UTC 的秒偏移(西半球为负)
bool isDSTInterval(unsigned long utcEpoch, int stdSec, int dstSec)
{
    int y, m, d;
    epochToYmd(utcEpoch, y, m, d);

    long long mar1 = daysFromCivil(y, 3, 1);
    long long nov1 = daysFromCivil(y, 11, 1);
    int wdMar = (int)((mar1 + 4) % 7);      // 1970-01-01 是周四, +4 使 0=周日
    int wdNov = (int)((nov1 + 4) % 7);
    long long secondSunMar = mar1 + ((7 - wdMar) % 7) + 7;   // 3 月第二个周日
    long long firstSunNov  = nov1 + ((7 - wdNov) % 7);       // 11 月第一个周日

    unsigned long start = (unsigned long)(secondSunMar * 86400LL) + (unsigned long)(7200 - stdSec);
    unsigned long end   = (unsigned long)(firstSunNov  * 86400LL) + (unsigned long)(7200 - dstSec);
    return (utcEpoch >= start) && (utcEpoch < end);
}

// IANA 时区名 -> 标准/夏令时偏移(秒)
void tzStdDst(const String& tz, int& stdSec, int& dstSec)
{
    if (tz.startsWith("America/Vancouver") || tz.startsWith("America/Los_Angeles")) {
        stdSec = -28800; dstSec = -25200;                 // 太平洋 -8/-7
    } else if (tz.startsWith("America/Edmonton") || tz.startsWith("America/Denver")) {
        stdSec = -25200; dstSec = -21600;                 // 山地 -7/-6
    } else if (tz.startsWith("America/Winnipeg") || tz.startsWith("America/Chicago")) {
        stdSec = -21600; dstSec = -18000;                 // 中部 -6/-5
    } else if (tz.startsWith("America/Halifax") || tz.startsWith("America/Glace_Bay") ||
               tz.startsWith("America/Moncton") || tz.startsWith("America/Bermuda")) {
        stdSec = -14400; dstSec = -10800;                 // 大西洋 -4/-3
    } else if (tz.startsWith("America/St_Johns")) {
        stdSec = -12600; dstSec = -9000;                  // 纽芬兰 -3:30/-2:30
    } else {
        stdSec = -18000; dstSec = -14400;                 // 东部(Toronto/New_York) -5/-4
    }
}

// 由 UTC 秒数返回该时区此刻应使用的秒偏移
int utcOffsetForZone(const String& tz, unsigned long utcEpoch)
{
    // 中国: 北京时间 UTC+8(无夏令时); 港澳台同为 +8; 新疆 IANA 标准为 +6
    if (tz.startsWith("Asia/Shanghai") || tz.startsWith("Asia/Chongqing") ||
        tz.startsWith("Asia/Harbin") || tz.startsWith("Asia/Hong_Kong") ||
        tz.startsWith("Asia/Macau") || tz.startsWith("Asia/Taipei")) return 28800;
    if (tz.startsWith("Asia/Urumqi")) return 21600;

    // 不使用夏令时的特例(北美)
    if (tz.startsWith("America/Fort_Nelson")) return -28800;                    // -8 固定
    if (tz.startsWith("America/Phoenix") || tz.startsWith("America/Creston") ||
        tz.startsWith("America/Dawson_Creek") || tz.startsWith("America/Whitehorse") ||
        tz.startsWith("America/Dawson")) return -25200;                          // -7 固定
    if (tz.startsWith("America/Regina") || tz.startsWith("America/Swift_Current"))
        return -21600;                                                          // -6 固定

    int stdSec, dstSec;
    tzStdDst(tz, stdSec, dstSec);
    return isDSTInterval(utcEpoch, stdSec, dstSec) ? dstSec : stdSec;
}

// 把算好的时区偏移写进 NTPClient(库里没有 getter, 偏移由 g_tzOffset 自己记录)
void syncLocalOffset()
{
    if (timeClient.getEpochTime() < 1000000000UL) return;    // NTP 尚未同步成功
    long cur = (long)timeClient.getEpochTime();              // 2026 年 < 2^31, 够用
    unsigned long utc = (unsigned long)(cur - g_tzOffset);   // 还原成 UTC 秒数
    int want = utcOffsetForZone(now_tz, utc);
    if (want != g_tzOffset) {
        timeClient.setTimeOffset(want);
        g_tzOffset = want;
        Serial.println("Timezone offset updated: " + String(want));
    }
}


/**************Setup***********************/
void setup()
{
    Serial.begin(115200);
    Serial.println("Start");
    tft.init();                         //初始化显示寄存器
    tft.fillScreen(TFT_WHITE);          //屏幕颜色
    tft.setTextColor(TFT_BLACK);        //设置字体颜色黑色
    tft.setCursor(15, 30, 1);           //设置文字开始坐标(15,30)及1号字体
    tft.setTextSize(1);
    
    tft.println("Connecting Wifi...");

    tft.setSwapBytes(true);             //使图片颜色由RGB->BGR
    get_wifi();                         // Wifi连接
    OTA::begin();                       // 启动 OTA 升级 (初始化 + 后台任务)

    detectLocationByIP();               // IP -> 城市/时区/经纬度(失败回退 Toronto)
    get_weather();                      // Open-Meteo 天气预报
    lastWeatherMs = millis();           // 计时起点: 10 分钟后首次自动刷新

    timeClient.begin();                 // NTP: time.cloudflare.com
    timeClient.setTimeOffset(0);        // 先按 UTC, 时区/DST 交给 syncLocalOffset()
    g_tzOffset = 0;
    timeClient.setUpdateInterval(3600000);  // NTP 每 1 小时校准一次
    lastTzSyncMs = millis();

    tft.fillScreen(TFT_BLACK);
    pageStartMs = millis();             // 页面调度计时起点
    astroTickMs = 0;                    // 0 让时间页在第一次循环立即绘制
}

/*******************时间数据刷新(动画帧刷新时调用)*******************/
void updateClockDisplay()
{
    unsigned long epochTime = timeClient.getEpochTime();
    epochToDate(epochTime);                  // 结果在 curYear / curMonth / curDay

    int h = timeClient.getHours();
    int m = timeClient.getMinutes();
    String hour   = (h < 10) ? "0" + String(h) : String(h);
    String minute = (m < 10) ? "0" + String(m) : String(m);
    dispTime = hour + ":" + minute;
    dispDate = String(curMonth) + "/" + String(curDay);
    dispYear = curYear;
    dispWeek = weekDays[timeClient.getDay()];
}

/*******************Loop*******************/

void loop()
{
    if (OTA::isActive()) {   // OTA 升级期间暂停主界面, 避免与升级画面抢占屏幕
        delay(10);
        return;
    }

    timeClient.update();               // NTP 库内部按需更新时间, 不阻塞
    unsigned long nowMs = millis();

    // 每分钟校正一次时区/DST(UTC 秒 -> 本地偏移, 跨 DST 自动切换)
    if (nowMs - lastTzSyncMs >= TZ_SYNC_MS) {
        lastTzSyncMs = nowMs;
        syncLocalOffset();
    }
    if (showWeatherNow) {
        // 天气页展示期间无需动画; 到期自动切回时间页
        if (nowMs - pageStartMs >= WEATHER_PAGE_MS) {
            showWeatherNow = false;
            pageStartMs = nowMs;
            astroTickMs = 0;           // 让时间页立刻重绘
            tft.fillScreen(TFT_BLACK);
        }
        delay(2);                      // 仅限流, 不影响任何动画计时
        return;
    }

    // 时间页到期 -> 切换并绘制天气页
    if (nowMs - pageStartMs >= TIME_PAGE_MS) {
        showWeatherNow = true;
        pageStartMs = nowMs;
        tft.fillScreen(TFT_BLACK);

        // 每 10 分钟在"页面切换(黑屏)"期间更新天气: 不会卡顿太空人动画
        if (nowMs - lastWeatherMs >= WEATHER_REFRESH_MS && WiFi.status() == WL_CONNECTED) {
            lastWeatherMs = nowMs;
            tft.setTextColor(TFT_WHITE, TFT_BLACK);
            tft.setCursor(20, 55, 1);
            tft.setTextSize(1);
            tft.println("Updating Weather...");
            get_weather();
        }

        show_weather(TFT_WHITE, TFT_BLACK);
        delay(2);
        return;
    }

    // 时间页: 按固定帧间隔推进太空人动画并刷新时间文本(millis 无阻塞)
    if (nowMs - astroTickMs >= ASTRO_FRAME_MS) {
        astroTickMs = nowMs;
        i += 1;
        if (i > 8) i = 0;
        updateClockDisplay();
        show_time(TFT_WHITE, TFT_BLACK, Astronaut,
                  dispTime, dispDate, dispYear, dispWeek.c_str());
    }

    delay(2);                          // 仅限流, 不影响任何动画计时
}
/*******************时间界面显示****************/
void show_time(uint16_t fg,uint16_t bg,const uint16_t* image[], String currentTime, String currentDate, int tm_Year,const char* week)
{
    //tft.fillRect(10, 55,  64, 64, bg);
    tft.setSwapBytes(true);             //使图片颜色由RGB->BGR
    tft.pushImage(10, 55,  64, 64, image[i]);
    tft.drawFastHLine(10, 53, 108, tft.alphaBlend(0, bg,  fg));
    showtext(15,5,2,3,fg,bg,currentTime);
    showtext(75,60,1,2,fg,bg, String(tm_Year));
    showtext(75,80,1,2,fg,bg, currentDate);
    showMyFonts(80, 100, week, TFT_YELLOW);
}

/*******************天气界面显示****************/
void show_weather(uint16_t fg,uint16_t bg)
{
    tft.setSwapBytes(true);             //使图片颜色由RGB->BGR
    tft.pushImage(5, 0,  64, 64, weather[ph]);

    // 右上角城市: 中文名(字库齐全)优先, 否则英文; 英文太长则显示省份
    String zhCity = cityDisplayString();
    if (zhCity.length() > 0) {
        showMyFonts(70, 8, zhCity.c_str(), TFT_WHITE);
    } else {
        String en = now_city;
        if (en.length() > 9) en = now_region.length() ? now_region : en.substring(0, 9);
        showtext(70, 12, 1, 1, fg, bg, en);
    }
    // 天气汉字(晴/多云/阴/雾/雨/雪/雷雨, 最多 2 字)
    // 雾/雷尚未加进字库时先用"阴/雨"代替, 加入后自动显示更准确的文字
    String cond = now_wea;
    if (cond.length() > 0 && !fontHasAllGlyphs(cond.c_str())) {
      if (cond == "雾") cond = "阴";
      else if (cond == "雷雨") cond = "雨";
    }
    if (cond.length() > 0) showMyFonts(70, 38, cond.c_str(), TFT_WHITE);

    tft.pushImage(0, 65, 30, 30, temIcon);
    tft.pushImage(0, 95, 30, 30, humIcon);
    tft.pushImage(55, 65, 30, 30, rainIcon);
    tft.pushImage(55, 95, 30, 30, windIcon);
    
    showtext(30,75,1,1,fg,bg,now_high_tem + "/" + now_low_tem);
    showtext(85,75,1,1,fg,bg,now_rainfall +"mm");
    showtext(30,105,1,1,fg,bg,now_hum+"%");
    showMyFonts(85, 100, now_wind_direction.c_str(), TFT_WHITE);
}

/*******************整句字符串显示****************/
void showtext(int16_t x,int16_t y,uint8_t font,uint8_t s,uint16_t fg,uint16_t bg,const String str)
{
  //设置文本显示坐标，和文本的字体，默认以左上角为参考点，
    tft.setCursor(x, y, font);
  // 设置文本颜色为白色，文本背景黑色
    tft.setTextColor(fg,bg);
  //设置文本大小，文本大小的范围是1-7的整数
    tft.setTextSize(s);
  // 设置显示的文字，注意这里有个换行符 \n 产生的效果
    tft.println(str);
}

/*******************单个汉字显示****************/
void showMyFont(int32_t x, int32_t y, const char c[3], uint32_t color) { 
  for (size_t k = 0; k < sizeof(hanzi) / sizeof(hanzi[0]); k++)  // 自动匹配字库字数
    if (hanzi[k].Index[0] == c[0] && hanzi[k].Index[1] == c[1] && hanzi[k].Index[2] == c[2])
    { tft.drawBitmap(x, y, hanzi[k].hz_Id, hanzi[k].hz_width, 16, color);
    }
}
/*******************整句汉字显示****************/
void showMyFonts(int32_t x, int32_t y, const char str[], uint32_t color) { //显示整句汉字，字库比较简单，上下、左右输出是在函数内实现
  int x0 = x;
  for (int i = 0; i < strlen(str); i += 3) {
    showMyFont(x0, y, str+i, color);
    x0 += 17;
  }
}
