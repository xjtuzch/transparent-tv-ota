/*
  ============================================================
  PomodoroDemo —— 透明小电视 · 番茄钟(独立工程)
  ============================================================
  功能:
    - 经典番茄钟: 专注 25 分钟 -> 短休息 5 分钟
    - 每完成 4 个专注进入长休息 15 分钟
    - 全程 millis 计时, 不阻塞; 状态变化/整秒变化才重绘
    - 屏幕按键提示 + 串口命令控制

  操作:
    BOOT 键(GPIO0) 短按  = 开始 / 暂停
    串口命令(115200):  s=开始  p=暂停  n=下一阶段  r=重置
    预留按键: BUTTON_NEXT_PIN 默认 -1(未接), 接上后短按 = 下一阶段

  屏幕(128x128, 深色极光风格):
    顶部: 阶段标签(FOCUS/BREAK) + 本轮完成圆点
    中部: 大字倒计时 MM:SS
    下方: 阶段进度条 + 操作提示
    阶段结束: 全屏闪烁 DONE 后自动进入下一阶段

  注意: 分光棱镜镜像显示时把 setup() 里的
  // tft.setRotation(4); 取消注释。
  ============================================================
*/

#include <TFT_eSPI.h>

// 阶段状态机(放最顶部: Arduino 会自动提升函数原型, 类型必须提前声明)
enum Phase {
  PHASE_IDLE,     // 待开始(显示完整专注时间)
  PHASE_FOCUS,    // 专注中
  PHASE_SHORT,    // 短休息
  PHASE_LONG,     // 长休息
  PHASE_DONE      // 阶段结束提示(短暂闪烁后自动进入下一阶段)
};

// ------------------- 番茄钟参数 -------------------
#define FOCUS_MIN       1     // 专注时长(分钟)
#define SHORT_BREAK_MIN 1      // 短休息时长(分钟)
#define LONG_BREAK_MIN  15     // 长休息时长(分钟)
#define FOCUS_PER_CYCLE 4      // 每几个专注后进入长休息

// ------------------- 输入引脚 -------------------
#define BUTTON_TOGGLE_PIN 0    // BOOT 键: 开始/暂停 (低电平有效)
#define BUTTON_NEXT_PIN   -1   // 预留"下一阶段"按键, 未接则 -1
#define DEBOUNCE_MS       50   // 按键消抖

// ------------------- 显示 -------------------
#define SW 128
#define SH 128

TFT_eSPI   tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);

// 低饱和极光配色(RGB888)
struct RGB3 { uint8_t r, g, b; };
uint16_t c3(const RGB3& c) { return tft.color565(c.r, c.g, c.b); }

const RGB3 COL_BG      = {9, 11, 20};      // 深蓝黑底
const RGB3 COL_TEXT    = {232, 240, 246};  // 近白
const RGB3 COL_DIM     = {120, 132, 146};  // 次要文字
const RGB3 COL_FOCUS   = {255, 142, 120};  // 专注: 珊瑚暖色
const RGB3 COL_BREAK   = {112, 222, 190};  // 休息: 薄荷青
const RGB3 COL_IDLE    = {150, 165, 180};  // 空闲: 灰蓝
const RGB3 COL_TRACK_F = {58, 38, 40};     // 专注进度条轨道
const RGB3 COL_TRACK_B = {26, 56, 50};     // 休息进度条轨道
const RGB3 COL_DONE_BG = {96, 26, 20};     // 完成闪烁底色

const unsigned long MS_FOCUS   = FOCUS_MIN * 60000UL;
const unsigned long MS_SHORT   = SHORT_BREAK_MIN * 60000UL;
const unsigned long MS_LONG    = LONG_BREAK_MIN * 60000UL;
const unsigned long DONE_MS    = 2600;      // 结束提示停留时长

Phase          phase      = PHASE_IDLE;   // 当前阶段
Phase          doneNext   = PHASE_FOCUS;  // DONE 结束后进入的阶段
unsigned long  remainingMs = MS_FOCUS;    // 本阶段剩余毫秒
unsigned long  lastTickMs  = 0;           // 上次计时推进时刻
unsigned long  doneUntilMs = 0;           // 结束提示截止时刻
bool           running    = false;        // 是否在倒计时
int            doneCount  = 0;            // 本轮已完成的专注数(0..FOCUS_PER_CYCLE)
int            lastShownSec = -1;         // 上次显示的剩余秒数(整秒变化才重绘)
unsigned long  lastFlashMs = 0;           // 结束闪烁上次绘制时刻

// 按钮状态
bool btnToggleLast = HIGH;
unsigned long btnToggleTime = 0;
bool btnNextLast = HIGH;
unsigned long btnNextTime = 0;

uint16_t col(RGB3 c) { return c3(c); }

// ------------------- 阶段工具 -------------------
const char* phaseName() {
  switch (phase) {
    case PHASE_FOCUS: return "FOCUS";
    case PHASE_SHORT: return "BREAK";
    case PHASE_LONG:  return "LONG BREAK";
    case PHASE_IDLE:  return "FOCUS";
    case PHASE_DONE:  return "DONE";
  }
  return "";
}

RGB3 accentColor() {
  switch (phase) {
    case PHASE_FOCUS:
    case PHASE_IDLE:  return running ? COL_FOCUS : COL_IDLE;
    case PHASE_SHORT:
    case PHASE_LONG:  return running ? COL_BREAK : COL_BREAK;
    case PHASE_DONE:  return COL_FOCUS;
  }
  return COL_IDLE;
}

RGB3 trackColor() {
  return (phase == PHASE_SHORT || phase == PHASE_LONG) ? COL_TRACK_B : COL_TRACK_F;
}

unsigned long phaseDuration(Phase p) {
  if (p == PHASE_FOCUS || p == PHASE_IDLE) return MS_FOCUS;
  if (p == PHASE_SHORT) return MS_SHORT;
  return MS_LONG;
}

// 自然完成当前阶段 -> 决定下一阶段(专注完成才计入轮次)
Phase nextPhaseAfterDone() {
  if (phase == PHASE_FOCUS) {
    doneCount++;
    if (doneCount >= FOCUS_PER_CYCLE) {
      doneCount = 0;
      return PHASE_LONG;
    }
    return PHASE_SHORT;
  }
  return PHASE_FOCUS;   // 任何休息结束都回到专注
}

void startPhase(Phase p) {
  phase = p;
  running = true;
  remainingMs = phaseDuration(p);
  lastTickMs = millis();
  lastShownSec = -1;
}

void goIdle() {
  phase = PHASE_IDLE;
  running = false;
  remainingMs = MS_FOCUS;
  doneCount = 0;
  lastShownSec = -1;
}

void toggleRun() {
  if (phase == PHASE_DONE) return;              // 结束提示期间忽略
  if (phase == PHASE_IDLE) {
    startPhase(PHASE_FOCUS);
    return;
  }
  if (running) {
    running = false;                            // 暂停(剩余时间保留)
    lastShownSec = -1;
  } else {
    running = true;
    lastTickMs = millis();
    lastShownSec = -1;
  }
}

// 跳过当前阶段(不计入完成的专注轮次)
void skipPhase() {
  if (phase == PHASE_DONE || phase == PHASE_IDLE) return;
  Phase next = (phase == PHASE_FOCUS) ? PHASE_SHORT : PHASE_FOCUS;
  if (phase == PHASE_FOCUS && doneCount >= FOCUS_PER_CYCLE - 1) next = PHASE_LONG;
  startPhase(next);
}

// ------------------- 绘制 -------------------
void drawDots(int cx, int cy) {
  int r = 3;
  for (int d = 0; d < FOCUS_PER_CYCLE; d++) {
    int x = cx + d * 10 - (FOCUS_PER_CYCLE - 1) * 5;
    RGB3 c = (d < doneCount) ? COL_BREAK : COL_DIM;
    spr.fillCircle(x, cy, r, col(c));
    if (d >= doneCount) spr.drawCircle(x, cy, r, col(c));   // 未完成画空圈
  }
}

void drawTimeText(int x, int y, const char* s) {
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(4);          // font1(6x8)放大4倍 => 24x32
  spr.setTextColor(col(COL_TEXT));
  spr.drawString(s, x, y, 1);
  spr.setTextSize(1);
  spr.setTextDatum(TL_DATUM);
}

void drawBar(int x, int y, int w, int h, float frac) {
  spr.fillRoundRect(x, y, w, h, h / 2, col(trackColor()));
  int fw = (int)(w * frac);
  if (fw > 4) spr.fillRoundRect(x, y, fw, h, h / 2, col(accentColor()));
}

void drawUI() {
  spr.fillSprite(col(COL_BG));

  // 顶部: 阶段标签 + 轮次圆点
  spr.setTextColor(col(accentColor()));
  spr.setTextSize(2);
  spr.setCursor(6, 6);
  spr.print(phaseName());
  spr.setTextSize(1);
  if (phase != PHASE_LONG) drawDots(106, 14);   // 长休标签占满整行, 不画圆点

  // 大字倒计时(MM:SS)
  unsigned long m = remainingMs / 60000UL;
  unsigned long s = (remainingMs % 60000UL) / 1000UL;
  String mm = (m < 10) ? "0" + String(m) : String(m);
  String ss = (s < 10) ? "0" + String(s) : String(s);
  String t  = mm + ":" + ss;
  drawTimeText(SW / 2, 58, t.c_str());

  // 进度条(已用比例)
  unsigned long total = phaseDuration(phase);
  float frac = (total > 0) ? 1.0f - (float)remainingMs / total : 0.0f;
  drawBar(10, 86, 108, 6, frac);

  // 底部操作提示
  spr.setTextColor(col(COL_DIM));
  spr.setTextSize(1);
  const char* hint = running ? "BOOT: PAUSE" : "BOOT: START";
  if (phase == PHASE_DONE) hint = "next phase...";
  spr.setCursor((SW - strlen(hint) * 6) / 2, 106);
  spr.print(hint);
  const char* hint2 = "serial: s p n r";
  spr.setCursor((SW - strlen(hint2) * 6) / 2, 118);
  spr.print(hint2);

  spr.pushSprite(0, 0);
}

// DONE 闪烁: 每 500ms 翻转一次 => 每秒闪烁一次
void drawDoneFlash(unsigned long nowMs) {
  bool on = ((nowMs / 1000) & 1) == 0;
  spr.fillSprite(on ? col(COL_DONE_BG) : col(COL_BG));
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(on ? col(COL_TEXT) : col(COL_FOCUS));
  spr.drawString("DONE", SW / 2, 52, 4);
  spr.setTextSize(1);
  spr.setTextColor(on ? col(COL_FOCUS) : col(COL_DIM));
  spr.drawString((doneNext == PHASE_FOCUS) ? "FOCUS NEXT" : "BREAK TIME",
                 SW / 2, 84, 2);
  spr.setTextSize(1);
  spr.setTextDatum(TL_DATUM);
  spr.pushSprite(0, 0);
}

void redrawIfNeeded(unsigned long nowMs) {
  if (phase == PHASE_DONE) {
    if (nowMs - lastFlashMs >= 250) {   // 节流重绘(不影响 1Hz 闪烁节奏)
      lastFlashMs = nowMs;
      drawDoneFlash(nowMs);   // 闪烁动画按时间自动重绘
    }
    return;
  }
  unsigned long sec = remainingMs / 1000UL;
  if (sec != lastShownSec) {
    lastShownSec = sec;
    drawUI();
  }
}

// ------------------- 按键与串口 -------------------
void handleButton(int pin, bool& lastState, unsigned long& lastTime,
                  void (*onPress)(), unsigned long nowMs) {
  if (pin < 0) return;
  bool level = digitalRead(pin);
  if (level != lastState) {
    lastTime = nowMs;              // 电平变化, 开始消抖计时
    lastState = level;
  } else if (level == LOW && nowMs - lastTime >= DEBOUNCE_MS) {
    lastTime = nowMs;              // 防重复触发
    onPress();
  }
}

void onTogglePress() { toggleRun(); }
void onNextPress()   { if (phase != PHASE_DONE && phase != PHASE_IDLE) skipPhase(); }

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 's') toggleRun();
    else if (c == 'p') { if (running) toggleRun(); }
    else if (c == 'n') { if (phase == PHASE_IDLE) startPhase(PHASE_FOCUS); else skipPhase(); }
    else if (c == 'r') goIdle();
  }
}

// ------------------- setup / loop -------------------
void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(0);       // 分光棱镜镜像显示时改成 tft.setRotation(4);
  spr.createSprite(SW, SH);
  if (!spr.created()) {
    Serial.println("Sprite create failed!");
    while (1) delay(100);
  }
  pinMode(BUTTON_TOGGLE_PIN, INPUT_PULLUP);
  if (BUTTON_NEXT_PIN >= 0) pinMode(BUTTON_NEXT_PIN, INPUT_PULLUP);
  goIdle();
  lastTickMs = millis();
  Serial.println("PomodoroDemo ready. s=start p=pause n=next r=reset");
}

void loop() {
  unsigned long nowMs = millis();
  handleSerial();
  handleButton(BUTTON_TOGGLE_PIN, btnToggleLast, btnToggleTime, onTogglePress, nowMs);
  handleButton(BUTTON_NEXT_PIN, btnNextLast, btnNextTime, onNextPress, nowMs);

  if (phase == PHASE_DONE) {
    if (nowMs >= doneUntilMs) startPhase(doneNext);   // 提示结束, 自动开始下一阶段
    redrawIfNeeded(nowMs);
    return;
  }

  if (running) {
    if (remainingMs > nowMs - lastTickMs) {
      remainingMs -= nowMs - lastTickMs;
    } else {
      remainingMs = 0;
      doneNext = nextPhaseAfterDone();
      phase = PHASE_DONE;
      running = false;
      doneUntilMs = nowMs + DONE_MS;
    }
    lastTickMs = nowMs;
  }

  redrawIfNeeded(nowMs);
  delay(5);                 // 限流, 不影响计时
}
