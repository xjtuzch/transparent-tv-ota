/*
  ============================================================
  EffectsDemo —— 透明小电视 · 四大特效演示(独立工程)
  ============================================================
  效果: 星空(穿越星流) / 粒子(余烬+萤火+爆裂) / 火焰 / 赛博雨
  全部为程序实时生成, 无需任何图片素材, 不占 flash 图片空间。

  使用方法:
  1. 把这个 EffectsDemo 文件夹当作独立 Arduino 工程打开
  2. 确认 TFT_eSPI 库的 User_Setup.h 已按你的屏幕配置好(ST7735 128x128)
  3. 编译烧录, 四个特效会循环播放

  提示: 若配合分光棱镜需要镜像显示, 把 setup() 里的
  // tft.setRotation(4);
  注释去掉(需要你的 TFT_eSPI 已添加 case4 镜像补丁)。
  ============================================================
*/

#include <TFT_eSPI.h>
#include <math.h>

// 颜色锚点(放最顶部: Arduino 会自动提升函数原型, 类型必须提前声明)
struct PCol { uint8_t r, g, b; };

TFT_eSPI   tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);   // 128x128 内存画布, 占用约 32KB RAM

// ------------------- 可调参数 -------------------
#define FRAME_MS   33       // 每帧间隔 ms(约30帧/秒)
#define EFFECT_MS  12000    // 每个特效持续 ms
#define TITLE_MS   1200     // 效果标题停留 ms
#define SW         128
#define SH         128

long frameCounter = 0;      // 全局帧计数(用于赛博雨抖动)

// 随机浮点: [a, b)
float frand(float a, float b) {
  return a + (b - a) * ((float)random(0, 10001) / 10000.0f);
}

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return tft.color565(r, g, b);
}

// ---------- 低饱和"深空极光"配色辅助 ----------
// 在若干颜色锚点之间线性插值
uint16_t pcol(float t, const PCol* stops, int n) {
  if (t <= 0.0f) return rgb(stops[0].r, stops[0].g, stops[0].b);
  if (t >= 1.0f) return rgb(stops[n - 1].r, stops[n - 1].g, stops[n - 1].b);
  float x = t * (n - 1);
  int i = (int)x;
  float f = x - i;
  const PCol& a = stops[i];
  const PCol& b = stops[i + 1];
  return rgb((uint8_t)(a.r + (b.r - a.r) * f),
             (uint8_t)(a.g + (b.g - a.g) * f),
             (uint8_t)(a.b + (b.b - a.b) * f));
}

// ============================================================
// 1) 星空: 背景闪烁星 + 穿越星流 + 偶尔流星
// ============================================================
#define N_WARP 100
#define N_TWI  40

float    warpX[N_WARP], warpY[N_WARP], warpZ[N_WARP];
uint8_t  twiX[N_TWI], twiY[N_TWI];
float    twiP[N_TWI];

bool          shootOn = false;
float         shX, shY, shVX, shVY;
int           shLife;
unsigned long lastShoot = 0;

void initStarfield() {
  for (int i = 0; i < N_WARP; i++) {
    warpX[i] = frand(-0.9f, 0.9f);
    warpY[i] = frand(-0.9f, 0.9f);
    warpZ[i] = frand(0.6f, 1.0f);
  }
  for (int i = 0; i < N_TWI; i++) {
    twiX[i] = random(0, SW);
    twiY[i] = random(0, SH);
    twiP[i] = frand(0.0f, 6.28f);
  }
  shootOn = false;
  lastShoot = 0;
}

void drawStarfieldFrame() {
  spr.fillSprite(TFT_BLACK);
  unsigned long ms = millis();
  int px, py;
  uint8_t b;

  // 背景闪烁星
  for (int i = 0; i < N_TWI; i++) {
    float v = 0.55f + 0.45f * sinf(ms * 0.004f + twiP[i]);
    b = (uint8_t)(v * 255.0f);
    spr.drawPixel(twiX[i], twiY[i], rgb(b, b, b));
  }

  // 穿越星流(星点向屏幕四周外飞, z 越小越近)
  for (int i = 0; i < N_WARP; i++) {
    warpZ[i] -= warpZ[i] * 0.045f + 0.006f;
    if (warpZ[i] < 0.15f) {
      warpX[i] = frand(-0.9f, 0.9f);
      warpY[i] = frand(-0.9f, 0.9f);
      warpZ[i] = 1.0f;
    }
    float k = 72.0f / warpZ[i];          // 投影放大系数
    px = SW / 2 + (int)(warpX[i] * k);
    py = SH / 2 + (int)(warpY[i] * k);
    if (px < 0 || px >= SW || py < 0 || py >= SH) continue;

    b = (uint8_t)min(255, (int)(k * 0.9f));   // 越近越亮
    if (warpZ[i] < 0.32f) {                   // 很近: 亮蓝白, 画成 2x2
      spr.fillRect(px - 1, py - 1, 2, 2, rgb(b, b, (uint8_t)min(255, b + 70)));
    } else {
      spr.drawPixel(px, py, rgb(b, b, b));
    }
  }

  // 流星: 大约每 2~4.5 秒随机出现一颗
  if (!shootOn && (ms - lastShoot) > (unsigned long)(2000 + random(0, 2500))) {
    shootOn = true;
    shX = frand(10.0f, SW * 0.6f);
    shY = frand(5.0f, SH * 0.3f);
    float a = frand(0.55f, 0.9f);
    shVX = cosf(a) * frand(1.6f, 2.3f);
    shVY = sinf(a) * frand(1.6f, 2.3f);
    shLife = 45;
  }
  if (shootOn) {
    for (int i = 0; i < 8; i++) {             // 尾巴(递减亮度)
      int tx = (int)(shX - shVX * 4.0f * i);
      int ty = (int)(shY - shVY * 4.0f * i);
      if (tx < 0 || tx >= SW || ty < 0 || ty >= SH) break;
      b = (uint8_t)max(0, 230 - i * 28);
      spr.drawPixel(tx, ty, rgb(b, b, (uint8_t)min(255, b + 70)));   // 冰蓝流星
    }
    shX += shVX; shY += shVY; shLife--;
    if (shLife <= 0 || shX > SW || shY > SH) {
      shootOn = false;
      lastShoot = ms;
    }
  }

  spr.pushSprite(0, 0);
}

// ============================================================
// 2) 粒子: 底部余烬上升 + 萤火虫漫游 + 周期性爆裂火花
// ============================================================
#define N_PART 150
#define RAMP_N 16
uint16_t rampWarm[RAMP_N];    // 余烬: 黑→红→橙→黄
uint16_t rampFly[RAMP_N];     // 萤火: 暗绿→亮绿黄
uint16_t rampCool[RAMP_N];    // 火花: 深蓝→青→白

struct Particle {
  float x, y, vx, vy;
  int   life, maxLife;
  uint8_t kind;               // 0=余烬 1=萤火 2=火花
};
Particle parts[N_PART];
unsigned long lastBurst = 0;

void initParticles() {
  for (int i = 0; i < N_PART; i++) {
    parts[i].kind = (i < 70) ? 0 : (i < 110) ? 1 : 2;
    parts[i].life = 0;        // 首帧全部重新生成
  }
  lastBurst = 0;
}

void spawnParticle(int i) {
  Particle& p = parts[i];
  p.maxLife = random(70, 190);
  p.life = p.maxLife;
  if (p.kind == 0) {          // 余烬: 屏幕底部随机升起
    p.x = frand(4, SW - 4);
    p.y = SH - 1;
    p.vx = frand(-0.25f, 0.25f);
    p.vy = -frand(0.5f, 1.3f);
  } else if (p.kind == 1) {   // 萤火: 全屏漫游
    p.x = frand(2, SW - 2);
    p.y = frand(2, SH - 2);
    p.vx = frand(-0.3f, 0.3f);
    p.vy = frand(-0.3f, 0.3f);
    p.maxLife = random(180, 320);
    p.life = p.maxLife;
  } else {                    // 火花: 从爆点向四周炸开(先按随机点)
    p.x = frand(15, SW - 15);
    p.y = frand(15, SH - 15);
    float a = frand(0, 6.2832f);
    float s = frand(0.7f, 2.4f);
    p.vx = cosf(a) * s;
    p.vy = sinf(a) * s - 0.6f;
    p.maxLife = random(50, 100);
    p.life = p.maxLife;
  }
}

void drawParticlesFrame() {
  spr.fillSprite(TFT_BLACK);
  unsigned long ms = millis();

  // 周期性爆裂, 给画面制造节奏
  if ((ms - lastBurst) > 900 + (unsigned long)random(0, 700)) {
    float bx = frand(30, SW - 30);
    float by = frand(30, SH - 30);
    for (int i = 0; i < N_PART; i++) {
      if (parts[i].kind == 2) {   // 所有火花集中到爆点重发
        Particle& p = parts[i];
        p.x = bx; p.y = by;
        float a = frand(0, 6.2832f);
        float s = frand(0.8f, 2.6f);
        p.vx = cosf(a) * s;
        p.vy = sinf(a) * s - 0.8f;
        p.maxLife = random(50, 110);
        p.life = p.maxLife;
      }
    }
    lastBurst = ms;
  }

  for (int i = 0; i < N_PART; i++) {
    Particle& p = parts[i];
    if (p.life <= 0) { spawnParticle(i); continue; }

    if (p.kind == 2) p.vy += 0.05f;              // 火花受重力
    if (p.kind == 1) {                           // 萤火随机转向漫游
      p.vx += frand(-0.06f, 0.06f);
      p.vy += frand(-0.06f, 0.06f);
      p.vx = constrain(p.vx, -0.6f, 0.6f);
      p.vy = constrain(p.vy, -0.6f, 0.6f);
    }
    p.x += p.vx; p.y += p.vy;
    p.life--;

    if (p.x < -2 || p.x > SW + 2 || p.y < -2 || p.y > SH + 2) { p.life = 0; continue; }

    // 颜色随寿命衰减
    uint16_t* ramp = (p.kind == 0) ? rampWarm : (p.kind == 1) ? rampFly : rampCool;
    int bi = (p.life * (RAMP_N - 1)) / p.maxLife;
    uint16_t col = ramp[bi];
    int xi = (int)p.x, yi = (int)p.y;
    spr.drawPixel(xi, yi, col);
    // 简单辉光: 四邻域用更暗一档的颜色
    uint16_t dim = ramp[max(0, bi - 4)];
    spr.drawPixel(xi - 1, yi, dim);
    spr.drawPixel(xi + 1, yi, dim);
    spr.drawPixel(xi, yi - 1, dim);
    spr.drawPixel(xi, yi + 1, dim);
  }
  spr.pushSprite(0, 0);
}

// ============================================================
// 3) 火焰: 64x64 扩散/冷却算法, 查调色板放大 2x 显示
// ============================================================
#define FW 64
#define FH 64
uint8_t  fireBuf[FW * FH];
uint16_t firePal[256];

void initFire() {
  memset(fireBuf, 0, sizeof(fireBuf));
  // 火焰配色: 炭黑 -> 暗红 -> 橙 -> 琥珀 -> 暖白(柔和有层次)
  static const PCol stops[] = {
    {8, 5, 5}, {118, 22, 16}, {214, 84, 24}, {250, 150, 62}, {255, 224, 168}, {255, 246, 232}
  };
  for (int v = 0; v < 256; v++) {
    firePal[v] = pcol(v / 255.0f, stops, 6);
  }
}

void drawFireFrame() {
  // 底部点火(留些黑缝更自然)
  for (int x = 0; x < FW; x++) {
    fireBuf[(FH - 1) * FW + x] = (random(0, 100) < 82) ? (uint8_t)random(150, 256) : 0;
  }
  // 自下而上扩散 + 冷却
  for (int y = FH - 2; y >= 0; y--) {
    for (int x = 0; x < FW; x++) {
      int xl = x > 0 ? x - 1 : 0;
      int xr = x < FW - 1 ? x + 1 : FW - 1;
      int v = (fireBuf[(y + 1) * FW + xl] + fireBuf[(y + 1) * FW + x] * 2
             + fireBuf[(y + 1) * FW + xr]) >> 2;
      if (random(0, 100) < 35) v--;                      // 随机冷却
      fireBuf[y * FW + x] = (uint8_t)max(0, v);
    }
  }
  // 放大 2 倍画到画布(2x2 色块)
  for (int y = 0; y < FH; y++) {
    for (int x = 0; x < FW; x++) {
      spr.fillRect(x * 2, y * 2, 2, 2, firePal[fireBuf[y * FW + x]]);
    }
  }
  spr.pushSprite(0, 0);
}

// ============================================================
// 4) 赛博雨: 每列一条绿色彗尾, 带随机抖动模拟字符下落
// ============================================================
#define NCOL 32        // 128 / 4 = 32 列
#define TRAIL 16       // 尾迹单元数
float   rainY[NCOL], rainS[NCOL];
uint8_t rainHue[NCOL];   // 0=绿 1=青

void initRain() {
  for (int c = 0; c < NCOL; c++) {
    rainY[c] = -frand(0, SH * 1.5f);
    rainS[c] = frand(0.9f, 2.2f);
    rainHue[c] = (random(0, 100) < 88) ? 0 : 1;
  }
}

uint16_t rainColor(uint8_t bri, uint8_t hue) {
  float x = bri / 255.0f;
  if (hue == 0) {   // 青绿(比纯绿更耐看)
    return rgb((uint8_t)(x * 70), (uint8_t)(x * 215), (uint8_t)(x * 190));
  }
  return rgb((uint8_t)(x * 190), (uint8_t)(x * 115), (uint8_t)(x * 205));  // 紫粉
}

void drawRainFrame() {
  spr.fillSprite(TFT_BLACK);
  for (int c = 0; c < NCOL; c++) {
    rainY[c] += rainS[c];
    if (rainY[c] > SH + TRAIL * 2) {                               // 落出屏幕重置
      rainY[c] = -frand(0, 70);
      rainS[c] = frand(0.9f, 2.2f);
    }
    int x0 = c * 4;
    int hy = (int)rainY[c];
    if (hy >= 0 && hy < SH) spr.fillRect(x0, hy, 4, 2, rgb(222, 255, 245));  // 雨头最亮
    for (int k = 1; k < TRAIL; k++) {                              // 尾迹向上衰减
      int yy = hy - k * 2;
      if (yy < 0) break;
      float t = (float)k / TRAIL;
      uint8_t bri = (uint8_t)(200.0f * (1.0f - t) * (1.0f - t));
      if (bri < 24) break;
      // 伪随机横向抖动, 模拟字符错落
      int jx = ((c * 37 + k * 11 + frameCounter / 3) % 5) - 2;
      int xx = x0 + jx;
      if (xx < 0) xx = 0;
      if (xx > SW - 2) xx = SW - 2;
      spr.fillRect(xx, yy, 2, 1, rainColor(bri, rainHue[c]));
    }
  }
  frameCounter++;
  spr.pushSprite(0, 0);
}

// ============================================================
// 通用: 标题 + 特效播放循环
// ============================================================
void showTitle(const char* s) {
  spr.fillSprite(TFT_BLACK);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(rgb(196, 228, 255), TFT_BLACK);   // 冰蓝白标题
  spr.drawString(s, SW / 2, SH / 2, 2);
  spr.setTextDatum(TL_DATUM);
  spr.pushSprite(0, 0);
  delay(TITLE_MS);
}

void runStarfield() {
  unsigned long t0 = millis(); initStarfield();
  while (millis() - t0 < EFFECT_MS) { drawStarfieldFrame(); delay(FRAME_MS); }
}
void runParticles() {
  unsigned long t0 = millis(); initParticles();
  while (millis() - t0 < EFFECT_MS) { drawParticlesFrame(); delay(FRAME_MS); }
}
void runFire() {
  unsigned long t0 = millis(); initFire();
  while (millis() - t0 < EFFECT_MS) { drawFireFrame(); delay(FRAME_MS); }
}
void runRain() {
  unsigned long t0 = millis(); initRain();
  while (millis() - t0 < EFFECT_MS) { drawRainFrame(); delay(FRAME_MS); }
}

// ============================================================
// 调色板预生成
// ============================================================
void buildPalettes() {
  // 余烬: 暗红 -> 橙 -> 暖琥珀
  static const PCol warm[] = {
    {14, 5, 7}, {120, 20, 24}, {214, 92, 36}, {255, 186, 108}
  };
  // 萤火: 深夜青 -> 薄荷绿
  static const PCol fly[] = {
    {6, 28, 26}, {66, 176, 138}, {196, 255, 224}
  };
  // 火花: 深靛 -> 蓝 -> 冰白
  static const PCol cool[] = {
    {16, 18, 54}, {92, 130, 232}, {214, 240, 255}
  };
  for (int i = 0; i < RAMP_N; i++) {
    float t = (float)i / (RAMP_N - 1);
    rampWarm[i] = pcol(t, warm, 4);
    rampFly[i]  = pcol(t, fly, 3);
    rampCool[i] = pcol(t, cool, 3);
  }
}

void setup() {
  Serial.begin(115200);
  randomSeed(analogRead(34));   // 用悬空 ADC 引脚做随机种子(每次开机画面不同)
  tft.init();
  tft.setRotation(0);           // 分光棱镜镜像显示时改成 tft.setRotation(4);
  spr.createSprite(SW, SH);
  if (!spr.created()) {
    Serial.println("Sprite create failed!");
    while (1) delay(100);
  }
  buildPalettes();
  Serial.println("EffectsDemo ready");
}

void loop() {
  showTitle("STARFIELD");
  runStarfield();
  showTitle("PARTICLES");
  runParticles();
  showTitle("FIRE");
  runFire();
  showTitle("CYBER RAIN");
  runRain();
}
