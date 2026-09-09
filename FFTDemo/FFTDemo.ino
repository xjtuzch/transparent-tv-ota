/*
  ============================================================
  FFTDemo —— 透明小电视 · 音乐频谱可视化(独立工程)
  ============================================================
  三种音源(改 SOUND_SRC 切换):
    0 = 内置合成电子节奏(无需麦克风, 纯看效果用)
    1 = INMP441 I2S 数字麦克风(当前默认, 音质干净)
    2 = MAX9814 / MAX4466 模拟麦克风(接 ADC)

  三种显示风格自动轮播:
    经典柱状 + 峰值线 / 上下镜像频谱 / 瀑布频谱

  原理:
    采样 256 点 -> 加汉宁窗 -> 浮点 FFT -> 按对数频段归并成 20 根柱
    -> 快速起跳 + 缓慢衰减平滑 -> 绘制

  接线(仅 SOUND_SRC=1 时需要):
    INMP441: VDD->3.3V  GND->GND  L/R->GND(左声道)
             SCK->GPIO14  WS->GPIO15  SD->GPIO32
    注1: L/R 接地 = 左声道; 若接 3.3V = 右声道(没声音时改接线或声道)。
    注2: GPIO15 是 ESP32 启动 strap 引脚(默认内部上拉), 正常使用没问题;
         若复位后无法启动, 把 WS 换到其他空闲引脚。
    注3: 不要和屏幕用的 18/19/23/25/26/27 冲突。

  接线(仅 SOUND_SRC=2 时需要):
    MAX9814/MAX4466: VCC->3.3V GND->GND OUT->GPIO34

  注意: 屏幕镜像(分光棱镜)时把 setup() 里的
  // tft.setRotation(4); 取消注释。
  ============================================================
*/

#include <TFT_eSPI.h>
#include <math.h>

// 颜色锚点(放最顶部: Arduino 会自动提升函数原型, 类型必须提前声明)
struct RGB3 { uint8_t r, g, b; };

// ------------------- 音源选择 -------------------
#ifndef SOUND_SRC
#define SOUND_SRC 1      // 0=合成 1=INMP441(I2S) 2=MAX9814/MAX4466(ADC)
#endif

#if SOUND_SRC == 1
#include <ESP_I2S.h>
#define I2S_BCK   14     // INMP441 SCK
#define I2S_WS    15     // INMP441 WS
#define I2S_DIN   32     // INMP441 SD
#define MIC_GAIN 3.0f    // 增益(按实际音量微调)
#endif

#if SOUND_SRC == 2
#define MIC_ADC_PIN 34   // 模拟麦 OUT
#define MIC_GAIN   6.0f  // 增益(按实际音量微调)
#endif

#if SOUND_SRC == 2
const int SAMPLE_RATE = 8000;    // ADC 采样率(analogRead 速度有限)
#else
const int SAMPLE_RATE = 16000;   // 合成 / I2S 采样率
#endif

// ---- 麦克风输入高通滤波(抑制直流漂移与低频环境噪声) ----
// 截止频率越高低频滤得越干净, 但会损失部分贝斯; 80Hz 是兼顾两者的取值
const float HP_CUTOFF = 0;
const float HP_ALPHA  = 1.0f / (1.0f + 6.2832f * HP_CUTOFF / SAMPLE_RATE);
float hpPrevX = 0.0f;            // 上一采样输入(跨帧保持)
float hpPrevY = 0.0f;            // 上一采样输出(跨帧保持)

// 一阶高通: y[n] = a*(y[n-1] + x[n] - x[n-1])
float hpFilter(float in) {
  float out = HP_ALPHA * (hpPrevY + in - hpPrevX);
  hpPrevX = in;
  hpPrevY = out;
  return out;
}

// ------------------- 显示参数 -------------------
#define FRAME_MS  33      // 每帧间隔 ms(~30帧/秒)
#define VIZ_MS    50000   // 每种风格展示时长 ms
#define TITLE_MS  900     // 标题停留 ms
#define SW        128
#define SH        128
#define NBAR      20      // 频谱柱数量(128/20, 每柱 6px)
#define FFT_N     256     // FFT 点数

TFT_eSPI   tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);

// 合成器音色状态(需放在文件顶部, 避免 Arduino 自动原型提升导致找不到类型)
struct Voice { float phase, env, freq, dec; };

// ------------------- FFT 资源 -------------------
float fftRe[FFT_N], fftIm[FFT_N];
float winT[FFT_N];                 // 汉宁窗
float twCos[FFT_N / 2], twSin[FFT_N / 2];
float mags[FFT_N / 2];

// ------------------- 频谱柱 -------------------
int    barBin0[NBAR], barBin1[NBAR];   // 每根柱覆盖的频点区间 [bin0, bin1)
float  barLevel[NBAR], barPeak[NBAR];
uint8_t wf[64][NBAR];                  // 瀑布历史(64 行 × 20 柱)
uint16_t wfPal[64];                    // 瀑布配色表

long frameCounter = 0;

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return tft.color565(r, g, b);
}

// ---------- 低饱和"深空极光"配色辅助 ----------
uint16_t bgCol;                       // 深色底(替代纯黑/纯色底)

// 在若干颜色锚点之间线性插值
uint16_t gradColor(float t, const RGB3* stops, int n) {
  if (t <= 0.0f) return rgb(stops[0].r, stops[0].g, stops[0].b);
  if (t >= 1.0f) return rgb(stops[n - 1].r, stops[n - 1].g, stops[n - 1].b);
  float x = t * (n - 1);
  int i = (int)x;
  float f = x - i;
  const RGB3& a = stops[i];
  const RGB3& b = stops[i + 1];
  return rgb((uint8_t)(a.r + (b.r - a.r) * f),
             (uint8_t)(a.g + (b.g - a.g) * f),
             (uint8_t)(a.b + (b.b - a.b) * f));
}

// ============================================================
// 浮点 FFT(基2 时域抽取, 自实现无需第三方库)
// ============================================================
void fftRun() {
  int n = FFT_N;
  // 位反转重排
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float t = fftRe[i]; fftRe[i] = fftRe[j]; fftRe[j] = t;
      t = fftIm[i];       fftIm[i] = fftIm[j]; fftIm[j] = t;
    }
  }
  // 蝶形运算
  for (int len = 2; len <= n; len <<= 1) {
    int half = len >> 1;
    int step = n / len;                 // 查表步长
    for (int i = 0; i < n; i += len) {
      for (int j = 0; j < half; j++) {
        int k = j * step;
        float c = twCos[k], s = twSin[k];
        int a = i + j, b = i + j + half;
        float tr = fftRe[b] * c + fftIm[b] * s;   // 乘 exp(-j*2*pi*k/n)
        float ti = fftIm[b] * c - fftRe[b] * s;
        fftRe[b] = fftRe[a] - tr;
        fftIm[b] = fftIm[a] - ti;
        fftRe[a] += tr;
        fftIm[a] += ti;
      }
    }
  }
}

// ============================================================
// 音源 A: 内置合成器(16 步电子节奏: 底鼓+贝斯+军鼓+踩镲)
// ============================================================
#if SOUND_SRC == 0
#define BPM 124
const uint32_t STEP_SAMPLES = (uint32_t)(SAMPLE_RATE * 60 / (BPM * 4));
const float bassSeq[8] = {55.0f, 55.0f, 65.41f, 49.0f,
                          55.0f, 73.42f, 65.41f, 49.0f};

Voice vKick, vBass, vSnare, vHat;
uint32_t synthPos = 0;
int stepIdx = -1;

void trigVoice(Voice& v, float f, float decaySec) {
  v.freq = f;
  v.env = 1.0f;
  v.dec = powf(0.001f, 1.0f / (decaySec * SAMPLE_RATE));
}

float synthTick() {
  uint32_t pos = synthPos++;
  int st = (int)(pos / STEP_SAMPLES);
  if (st != stepIdx) {
    stepIdx = st;
    int s = st % 16;
    if (s == 0 || s == 4 || s == 8 || s == 12) trigVoice(vKick, 58.0f, 0.14f);
    if (s == 2 || s == 6 || s == 10 || s == 14) trigVoice(vSnare, 0.0f, 0.10f);
    if ((s & 1) == 1) trigVoice(vHat, 0.0f, 0.04f);
    if (s == 0 || s == 3 || s == 6 || s == 8 || s == 11 || s == 14) {
      trigVoice(vBass, bassSeq[(st / 16) % 8], 0.20f);
    }
  }
  float out = 0;
  if (vKick.env > 0.001f) {
    vKick.phase += 6.2832f * vKick.freq / SAMPLE_RATE;
    out += sinf(vKick.phase) * vKick.env;
    vKick.env *= vKick.dec;
  }
  if (vBass.env > 0.001f) {
    vBass.phase += 6.2832f * vBass.freq / SAMPLE_RATE;
    out += (sinf(vBass.phase) + 0.35f * sinf(2.0f * vBass.phase)) * 0.7f * vBass.env;
    vBass.env *= vBass.dec;
  }
  // 确定性伪噪声(避免每采样调 random 的开销)
  uint32_t nn = pos * 2654435761u;
  float noise = ((float)((nn >> 24) & 0xFF) / 127.5f) - 1.0f;
  if (vSnare.env > 0.001f) { out += noise * 0.75f * vSnare.env; vSnare.env *= vSnare.dec; }
  if (vHat.env   > 0.001f) { out += noise * 0.35f * vHat.env;   vHat.env   *= vHat.dec; }
  out *= 0.55f;
  if (out > 1.0f) out = 1.0f;
  if (out < -1.0f) out = -1.0f;
  return out;
}

void captureAudio() {
  for (int i = 0; i < FFT_N; i++) fftRe[i] = synthTick();
}
#endif

// ============================================================
// 音源 B: INMP441 I2S 数字麦克风
// ============================================================
#if SOUND_SRC == 1
I2SClass mic;

void initMic() {
  mic.setPins(I2S_BCK, I2S_WS, -1, I2S_DIN);
  if (!mic.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT,
                 I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("I2S init failed!");
    while (1) delay(100);
  }
  mic.setTimeout(20);
  Serial.print("INMP441 pins: SCK="); Serial.print(I2S_BCK);
  Serial.print(" WS="); Serial.print(I2S_WS);
  Serial.print(" SD="); Serial.print(I2S_DIN);
  Serial.print(" @"); Serial.print(SAMPLE_RATE); Serial.println("Hz");
}

void captureAudio() {
  int32_t raw[FFT_N];
  memset(raw, 0, sizeof(raw));          // 先清零, 防止超时只读到部分数据时尾段是垃圾值
  size_t need = sizeof(raw), got = 0;
  unsigned long t0 = millis();
  while (got < need && millis() - t0 < 60) {
    got += mic.readBytes((char*)raw + got, need - got);
  }
  float mean = 0;
  for (int i = 0; i < FFT_N; i++) {
    // 24bit 左对齐 -> 归一 -> 高通滤除直流/低频噪声 -> 增益
    float sample = (float)(raw[i] >> 14) * (1.0f / 16384.0f);
    fftRe[i] = hpFilter(sample) * MIC_GAIN;
    mean += fftRe[i];
  }
  mean /= FFT_N;                       // 去直流
  for (int i = 0; i < FFT_N; i++) fftRe[i] -= mean;
}
#endif

// ============================================================
// 音源 C: MAX9814/MAX4466 模拟麦克风(ADC 采样)
// ============================================================
#if SOUND_SRC == 2
void initMic() {
  // 默认 11dB 衰减即可; 若要更大输入范围可调衰减
}

void captureAudio() {
  unsigned long next = micros();
  const unsigned long period = 1000000UL / SAMPLE_RATE;
  for (int i = 0; i < FFT_N; i++) {
    while ((long)(micros() - next) < 0) {}
    next += period;
    float sample = (analogRead(MIC_ADC_PIN) - 2048) * (1.0f / 2048.0f);
    fftRe[i] = hpFilter(sample) * MIC_GAIN;   // 高通滤除偏置漂移/低频噪声
  }
  float mean = 0;
  for (int i = 0; i < FFT_N; i++) mean += fftRe[i];
  mean /= FFT_N;                       // 去直流(麦克风输出有 1/2 VCC 偏置)
  for (int i = 0; i < FFT_N; i++) fftRe[i] -= mean;
}
#endif

// ============================================================
// 分析一帧: 采样 -> 加窗 -> FFT -> 频段归并 -> 平滑
// ============================================================
void analyzeFrame() {
  captureAudio();
  for (int i = 0; i < FFT_N; i++) {
    fftRe[i] *= winT[i];
    fftIm[i] = 0;
  }
  fftRun();
  for (int k = 0; k < FFT_N / 2; k++) {
    mags[k] = sqrtf(fftRe[k] * fftRe[k] + fftIm[k] * fftIm[k]) * (2.0f / FFT_N);
    if (mags[k] > 1.0f) mags[k] = 1.0f;
  }

  for (int b = 0; b < NBAR; b++) {
    float amp = 0;
    int cnt = 0;
    for (int k = barBin0[b]; k < barBin1[b]; k++) { amp += mags[k]; cnt++; }
    if (cnt > 0) amp /= cnt;
    if (amp > 1.0f) amp = 1.0f;

    // 线性幅度 -> dB(-50~0dB) -> 0~1
    float db = 20.0f * log10f(amp + 1e-4f);
    float v = (db + 50.0f) / 50.0f;
    if (v < 0) v = 0; else if (v > 1) v = 1;

    // 快速起跳 + 缓慢衰减
    if (v > barLevel[b]) barLevel[b] = v;
    else barLevel[b] *= 0.90f;
    if (barLevel[b] < 0.002f) barLevel[b] = 0;

    if (v > barPeak[b]) barPeak[b] = v;
    else barPeak[b] *= 0.985f;
    if (barPeak[b] < 0.01f) barPeak[b] = 0;
  }
}

// ============================================================
// 颜色工具
// ============================================================
// 频谱柱: 深靛 -> 紫罗兰 -> 粉 -> 暖金(低饱和, 有层次)
uint16_t levelColor(float t) {
  static const RGB3 stops[] = {
    // {38, 30, 92},    // 深靛(低音量, 若隐若现)
    // {96, 62, 180},   // 紫罗兰
    // {176, 96, 214},  // 亮紫
    // {242, 130, 192}, // 粉
    // {255, 208, 155}  // 暖金(最高, 柔和不刺眼)

    //{25,55,120},{55,120,210},{110,195,240},{185,240,252},{235,252,255}//蓝
    {20,70,80},{40,140,130},{90,200,170},{170,240,210},{235,255,240}//绿
    //{70,25,20},{150,55,30},{220,110,40},{255,175,70},{255,225,170}
  };
  return gradColor(t, stops, 5);
}

// 瀑布配色: 墨蓝夜空 -> 紫 -> 粉 -> 暖金(有纵深的"极光"感)
void buildWfPalette() {
  static const RGB3 stops[] = {
    // {10, 9, 26},     // 夜空底色
    // {52, 38, 116},   // 深紫
    // {124, 80, 208},  // 紫罗兰
    // {210, 128, 220}, // 淡紫粉
    // {255, 175, 165}, // 暖珊瑚
    // {255, 228, 178}  // 香槟金

    {6,14,16},{20,60,75},{45,130,140},{110,205,180},{190,245,220},{240,255,245}//绿
    //{20,8,8},{80,25,18},{170,70,30},{240,140,55},{255,205,120},{255,240,210}
  };
  for (int i = 0; i < 64; i++) {
    wfPal[i] = gradColor(i / 63.0f, stops, 6);
  }
}

// ============================================================
// 三种显示风格
// ============================================================
void drawBars() {          // 风格1: 底部柱状 + 白色峰值线
  spr.fillSprite(bgCol);
  int bw = 6, x0 = 4;
  for (int b = 0; b < NBAR; b++) {
    int h = (int)(barLevel[b] * 118.0f);
    if (h > 0) {
      spr.fillRect(x0 + b * bw, 126 - h, bw - 1, h, levelColor(barLevel[b]));
    }
    int py = 126 - (int)(barPeak[b] * 118.0f);
    spr.drawFastHLine(x0 + b * bw, py, bw - 1, rgb(255, 238, 210));
  }
}

void drawMirror() {        // 风格2: 从中线向上下镜像
  spr.fillSprite(bgCol);
  spr.drawFastHLine(0, 63, SW, rgb(48, 40, 104));
  int bw = 6, x0 = 4;
  for (int b = 0; b < NBAR; b++) {
    int h = (int)(barLevel[b] * 62.0f);
    uint16_t col = levelColor(barLevel[b]);
    if (h > 0) spr.fillRect(x0 + b * bw, 63 - h, bw - 1, h * 2, col);
    int pk = (int)(barPeak[b] * 62.0f);
    spr.drawFastHLine(x0 + b * bw, 63 - pk, bw - 1, rgb(255, 238, 210));
    spr.drawFastHLine(x0 + b * bw, 63 + pk, bw - 1, rgb(255, 238, 210));
  }
}

void drawWaterfall() {     // 风格3: 频谱瀑布(新数据在底部, 历史向上滚)
  memmove(&wf[0][0], &wf[1][0], (64 - 1) * NBAR);
  for (int b = 0; b < NBAR; b++) {
    wf[63][b] = (uint8_t)(barLevel[b] * 63.0f + 0.5f);
  }
  spr.fillSprite(bgCol);
  int bw = 6, x0 = 4;
  for (int r = 0; r < 64; r++) {
    for (int b = 0; b < NBAR; b++) {
      spr.fillRect(x0 + b * bw, r * 2, bw - 1, 2, wfPal[wf[r][b]]);
    }
  }
}

// ============================================================
// 流程控制
// ============================================================
void showTitle(const char* s) {
  spr.fillSprite(bgCol);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(rgb(196, 228, 255), bgCol);   // 冰蓝白标题
  spr.drawString(s, SW / 2, SH / 2, 2);
  spr.setTextDatum(TL_DATUM);
  spr.pushSprite(0, 0);
  delay(TITLE_MS);
}

void runStyle(int style) {
  unsigned long t0 = millis();
  while (millis() - t0 < VIZ_MS) {
    analyzeFrame();
    if (style == 0) drawBars();
    else if (style == 1) drawMirror();
    else drawWaterfall();
    spr.pushSprite(0, 0);
    delay(FRAME_MS);
  }
}

// ============================================================
// setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  randomSeed(analogRead(34));
  tft.init();
  //tft.setRotation(4);          // 分光棱镜镜像显示时改成 tft.setRotation(4);
  spr.createSprite(SW, SH);
  if (!spr.created()) {
    Serial.println("Sprite create failed!");
    while (1) delay(100);
  }
  bgCol = tft.color565(6,14,13);     // 深蓝黑底色(比纯黑更有层次)

  // 汉宁窗
  for (int i = 0; i < FFT_N; i++) {
    winT[i] = 0.5f - 0.5f * cosf(6.2832f * i / (FFT_N - 1));
  }
  // 旋转因子表 W = exp(-j*2*pi*k/FFT_N)
  for (int k = 0; k < FFT_N / 2; k++) {
    float a = 6.2832f * k / FFT_N;
    twCos[k] = cosf(a);
    twSin[k] = sinf(a);        // 配合 fftRun 中的公式取负号相位
  }
  // 对数频段映射(60Hz ~ Nyquist)
  float fLow = 60.0f, fHigh = SAMPLE_RATE / 2.0f;
  for (int b = 0; b < NBAR; b++) {
    float f0 = fLow * powf(fHigh / fLow, (float)b / NBAR);
    float f1 = fLow * powf(fHigh / fLow, (float)(b + 1) / NBAR);
    int s = (int)(f0 * FFT_N / SAMPLE_RATE);
    int e = (int)(f1 * FFT_N / SAMPLE_RATE) + 1;
    if (s < 1) s = 1;
    if (e > FFT_N / 2) e = FFT_N / 2;
    if (e <= s) e = s + 1;
    barBin0[b] = s; barBin1[b] = e;
  }
  memset(barLevel, 0, sizeof(barLevel));
  memset(barPeak, 0, sizeof(barPeak));
  memset(wf, 0, sizeof(wf));
  buildWfPalette();

#if SOUND_SRC == 1 || SOUND_SRC == 2
  initMic();
#endif

  Serial.print("FFTDemo ready, SRC=");
#if SOUND_SRC == 0
  Serial.println("SYNTH");
#elif SOUND_SRC == 1
  Serial.println("INMP441");
#else
  Serial.println("ADC");
#endif
}

void loop() {
  //showTitle("BARS");
  delay(200);
  runStyle(0);
  //showTitle("MIRROR");
  delay(200);
  runStyle(1);
  //showTitle("WATERFALL");
  delay(200);
  runStyle(2);
}
