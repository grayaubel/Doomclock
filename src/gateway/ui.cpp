// ui.cpp — screens on the ILI9488 (480x320 landscape) with XPT2046 touch, via TFT_eSPI.
// Text is English because TFT_eSPI's built-in fonts have no Thai glyphs.
// Only the parts that changed are redrawn (an 18-bit SPI panel is slow to repaint fully).
// Home screen = flip clock (see the "home: flip clock" section).
#include "ui.h"
#include <TFT_eSPI.h>
#include <Preferences.h>
#include <math.h>
#include "flip_font.h"

static TFT_eSPI tft;
static Preferences uiPrefs;
static uint16_t calData[5];
static bool haveCal = false;

static UiScreen screen = SCR_HOME;
static bool fullRedraw = true;
static Settings work;              // working copy while editing
static int sel = -1;               // selected field: 0..6 = day time, 7 = sleep goal

// colours
static uint16_t C_BG, C_CARD, C_DIGIT, C_ORANGE, C_MUTED, C_TEXT, C_GREEN, C_RED, C_BLUE, C_PANEL;

// touch targets rebuilt on every full draw
struct Btn { int16_t x, y, w, h; uint8_t action; int8_t arg; };
static Btn btns[28];
static int nBtns = 0;
static bool touchWasDown = false;

static const char* DAYS[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
static const char* MONTHS[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

enum : uint8_t { BA_ACTION = 0, BA_SEL_DAY = 100, BA_TOGGLE_RING = 101, BA_SEL_GOAL = 102, BA_TOGGLE_DATA = 103 };

static void addBtn(int x, int y, int w, int h, uint8_t action, int8_t arg = 0) {
  if (nBtns < 28) btns[nBtns++] = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, action, arg};
}

static void button(int x, int y, int w, int h, const char* label, uint16_t fill, uint16_t text, uint8_t action, int8_t arg = 0) {
  tft.fillRoundRect(x, y, w, h, 8, fill);
  tft.setTextColor(text, fill);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(label, x + w / 2, y + h / 2 + 1, 4);
  addBtn(x, y, w, h, action, arg);
}

void uiBegin() {
  tft.init();
  tft.setRotation(1);                       // 480 x 320 landscape
  C_BG = TFT_BLACK;
  C_CARD = tft.color565(52, 52, 52);
  C_DIGIT = tft.color565(246, 239, 226);
  C_ORANGE = tft.color565(244, 162, 89);
  C_MUTED = tft.color565(140, 140, 140);
  C_TEXT = tft.color565(235, 235, 235);
  C_GREEN = tft.color565(80, 200, 120);
  C_RED = tft.color565(230, 80, 70);
  C_BLUE = tft.color565(91, 141, 239);
  C_PANEL = tft.color565(28, 28, 28);
  tft.fillScreen(C_BG);
  uiPrefs.begin("dcui", false);
  if (uiPrefs.getBytesLength("tcal") == sizeof(calData)) {
    uiPrefs.getBytes("tcal", calData, sizeof(calData));
    tft.setTouch(calData);
    haveCal = true;
  }
}

bool uiHasCalibration() { return haveCal; }

void uiCalibrate() {
  tft.fillScreen(C_BG);
  tft.setTextColor(C_TEXT, C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Touch calibration", 240, 120, 4);
  tft.drawString("Tap each corner arrow as it appears", 240, 160, 2);
  delay(1200);
  tft.calibrateTouch(calData, C_ORANGE, C_BG, 15);
  uiPrefs.putBytes("tcal", calData, sizeof(calData));
  haveCal = true;
  fullRedraw = true;
}

void uiShow(UiScreen s) {
  if (s == SCR_SETTINGS || s == SCR_SETUP) { work = gSettings; sel = -1; }
  screen = s;
  fullRedraw = true;
}

UiScreen uiScreen() { return screen; }

void uiKnob(int d) {
  if (screen != SCR_SETTINGS && screen != SCR_SETUP) return;
  if (sel >= 0 && sel < 7) {
    int m = work.day[sel].hour * 60 + work.day[sel].minute + d * 5;
    m = (m % 1440 + 1440) % 1440;
    work.day[sel].hour = m / 60;
    work.day[sel].minute = m % 60;
  } else if (sel == 7) {
    int g = (int)work.sleepGoalMin + d * 15;
    work.sleepGoalMin = g < 240 ? 240 : (g > 720 ? 720 : g);
  }
  fullRedraw = true;
}

void uiKnobPress() {
  if (screen != SCR_SETTINGS && screen != SCR_SETUP) return;
  sel = (sel + 1) % 8;
  fullRedraw = true;
}

UiAction uiPollTouch() {
  uint16_t x, y;
  bool down = haveCal && tft.getTouch(&x, &y, 500);
  bool tap = down && !touchWasDown;
  touchWasDown = down;
  if (!tap) return UA_NONE;
  for (int i = 0; i < nBtns; i++) {
    const Btn& b = btns[i];
    if (x < b.x || x >= b.x + b.w || y < b.y || y >= b.y + b.h) continue;
    switch (b.action) {
      case BA_SEL_DAY:  sel = b.arg; fullRedraw = true; return UA_NONE;
      case BA_SEL_GOAL: sel = 7; fullRedraw = true; return UA_NONE;
      case BA_TOGGLE_RING: work.day[b.arg].ring ^= 1; fullRedraw = true; return UA_NONE;
      case BA_TOGGLE_DATA: work.dataMode ^= 1; fullRedraw = true; return UA_NONE;
      default: break;
    }
    UiAction a = (UiAction)b.action;
    if (a == UA_SAVED) { gSettings = work; settingsSave(); uiShow(SCR_HOME); }
    else if (a == UA_CANCEL) uiShow(SCR_HOME);
    else if (a == UA_OPEN_SETTINGS) uiShow(SCR_SETTINGS);
    return a;
  }
  return UA_NONE;
}

// ---------------- settings / setup ----------------
static void drawSettings(bool setup) {
  tft.fillScreen(C_BG);
  nBtns = 0;
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_ORANGE, C_BG);
  tft.drawString(setup ? "Setup: wake time for every day" : "Wake schedule", 14, 8, 4);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString("Tap a time, turn the knob (5 min). Knob press = next field.", 14, 36, 2);
  char buf[24];
  for (int d = 0; d < 7; d++) {
    int y = 58 + d * 30;
    bool s = (sel == d);
    tft.setTextColor(C_TEXT, C_BG);
    tft.drawString(DAYS[d], 18, y + 6, 2);
    uint16_t f = s ? C_ORANGE : C_CARD;
    tft.fillRoundRect(70, y, 96, 26, 6, f);
    snprintf(buf, sizeof(buf), "%02d:%02d", work.day[d].hour, work.day[d].minute);
    tft.setTextColor(s ? TFT_BLACK : C_DIGIT, f);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(buf, 118, y + 14, 4);
    addBtn(70, y, 96, 26, BA_SEL_DAY, d);
    uint16_t rf = work.day[d].ring ? C_BLUE : C_CARD;
    tft.fillRoundRect(176, y, 90, 26, 6, rf);
    tft.setTextColor(C_TEXT, rf);
    tft.drawString(work.day[d].ring ? "RING" : "SILENT", 221, y + 14, 2);
    addBtn(176, y, 90, 26, BA_TOGGLE_RING, d);
    tft.setTextDatum(TL_DATUM);
  }
  // right column
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString("Sleep goal", 300, 62, 2);
  uint16_t gf = (sel == 7) ? C_ORANGE : C_CARD;
  tft.fillRoundRect(300, 82, 160, 34, 6, gf);
  snprintf(buf, sizeof(buf), "%dh%02d", work.sleepGoalMin / 60, work.sleepGoalMin % 60);
  tft.setTextColor(sel == 7 ? TFT_BLACK : C_DIGIT, gf);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(buf, 380, 100, 4);
  addBtn(300, 82, 160, 34, BA_SEL_GOAL);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString("Data collection", 300, 128, 2);
  uint16_t df = work.dataMode ? C_GREEN : C_CARD;
  tft.fillRoundRect(300, 148, 160, 30, 6, df);
  tft.setTextColor(TFT_BLACK, df);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(work.dataMode ? "ON" : "OFF", 380, 163, 4);
  addBtn(300, 148, 160, 30, BA_TOGGLE_DATA);
  button(300, 190, 160, 30, "Touch cal", C_CARD, C_TEXT, UA_TOUCH_CAL);
  button(300, 272, 160, 40, "SAVE", C_ORANGE, TFT_BLACK, UA_SAVED);
  if (!setup) button(300, 228, 160, 36, "Cancel", C_CARD, C_TEXT, UA_CANCEL);
}

// ---------------- home: flip clock ----------------
// Two dark cards (HH, MM) with large anti-aliased digits (flip_font.h, generated by
// tools/fonts/make_flip_font.py), a warm glow from the bottom, the next wake time in a pill,
// buttons on the right of the bottom row and one status line under it.
// Cards, background, pill and buttons are drawn by per-pixel functions into a 16-row band
// buffer (no full-screen sprite: no RAM for one) and dithered to RGB565 so gradients don't band.
// A card flip advances one frame per uiRender() call, so TaskApp's 20 ms tick never stalls.

#define H_CARD_W    200
#define H_CARD_H    213
#define H_CARD_Y    30
#define H_CARD_R    12.0f
#define H_HALF      (H_CARD_H / 2)
static const int H_CARD_X[2] = {12, 268};
#define H_COLON_X   240
#define H_COLON_DY  22
#define H_COLON_R   9.0f
#define H_ROW_Y     254                // bottom row: wake pill + buttons
#define H_ROW_H     43
#define H_PILL_X    12
#define H_PILL_R    10.0f
#define H_PILL_PAD  10
#define H_ICON_R    8.5f
#define H_BOTTOM_Y  246                // everything below the cards is redrawn together
#define H_STATUS_Y  302
#define H_FLIP_FRAMES 7                // frames per half-flip
#define BIG_DASH    10                 // '-' in BIG_GLYPHS
#define SMALL_COLON 10
#define SMALL_DASH  11

struct RGB { float r, g, b; };
static const RGB H_BG = {20, 20, 20}, H_GLOW = {72, 58, 50};
static const RGB H_CARD = {19, 19, 19}, H_BORDER = {42, 42, 42}, H_WHITE = {255, 255, 255};
static const RGB H_PILL_FG = {28, 20, 26}, H_TRACK = {48, 48, 48};
static const RGB H_GREEN = {80, 200, 120}, H_RED = {230, 80, 70};

static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static inline RGB mix(RGB a, RGB b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
static inline RGB scale(RGB a, float k) { return {a.r * k, a.g * k, a.b * k}; }
static inline uint16_t rgb565(RGB c) { return tft.color565((uint8_t)c.r, (uint8_t)c.g, (uint8_t)c.b); }

static inline float sdRoundRect(float px, float py, float x, float y, float w, float h, float r) {
  float hw = w * 0.5f, hh = h * 0.5f;
  float qx = fabsf(px - (x + hw)) - (hw - r), qy = fabsf(py - (y + hh)) - (hh - r);
  float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, in = qx > qy ? qx : qy;
  return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}

static inline float sdSegment(float px, float py, float ax, float ay, float bx, float by) {
  float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
  float h = clamp01((pax * bax + pay * bay) / (bax * bax + bay * bay));
  float dx = pax - bax * h, dy = pay - bay * h;
  return sqrtf(dx * dx + dy * dy);
}

static inline float glyphAlpha(const FlipGlyph* G, const uint8_t* data, int idx, int lx, int ly) {
  const FlipGlyph& g = G[idx];
  lx -= g.x; ly -= g.y;
  if ((unsigned)lx >= g.w || (unsigned)ly >= g.h) return 0;
  return data[g.ofs + ly * g.w + lx] * (1.0f / 255.0f);
}

static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
static inline uint16_t to565(RGB c, int x, int y) {
  float t = (BAYER[y & 3][x & 3] + 0.5f) / 16.0f - 0.5f;          // ordered dither, +-0.47 of one step
  int r = (int)floorf(c.r * (31.0f / 255.0f) + t + 0.5f);
  int g = (int)floorf(c.g * (63.0f / 255.0f) + t + 0.5f);
  int b = (int)floorf(c.b * (31.0f / 255.0f) + t + 0.5f);
  r = r < 0 ? 0 : r > 31 ? 31 : r;
  g = g < 0 ? 0 : g > 63 ? 63 : g;
  b = b < 0 ? 0 : b > 31 ? 31 : b;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

#define BAND_ROWS 16
static uint16_t band[480 * BAND_ROWS];

template <typename F>
static void renderRect(int x0, int y0, int w, int h, F fn) {
  tft.setSwapBytes(true);
  for (int by = 0; by < h; by += BAND_ROWS) {
    int rows = h - by < BAND_ROWS ? h - by : BAND_ROWS;
    uint16_t* p = band;
    for (int r = 0; r < rows; r++) {
      int y = y0 + by + r;
      for (int x = x0; x < x0 + w; x++) *p++ = to565(fn(x, y), x, y);
    }
    tft.pushImage(x0, y0 + by, w, rows, band);
  }
  tft.setSwapBytes(false);
}

static RGB hBackground(int x, int y) {
  float dx = (x - 240) / 300.0f, dy = (y - 340) / 210.0f;
  float t = clamp01(1.0f - sqrtf(dx * dx + dy * dy));
  return mix(H_BG, H_GLOW, t * t * (3 - 2 * t));
}

// ---- cards ----
// d[4] = H H M M, -1 = unknown ('-').
static int8_t shownD[4] = {-2, -2, -2, -2};

// Card i over `under`, sampled at (x, y) as if it held digits d. shade < 1 darkens it (a turning flap).
static RGB cardLayer(int i, const int8_t* d, int x, int y, RGB under, float shade) {
  float s = sdRoundRect(x + 0.5f, y + 0.5f, H_CARD_X[i], H_CARD_Y, H_CARD_W, H_CARD_H, H_CARD_R);
  if (s > 0.5f) return under;
  RGB c = mix(H_CARD, H_BORDER, clamp01(1.0f - fabsf(s + 0.5f)));        // 1 px border
  const int adv = BIG_GLYPHS[0].adv;                                       // digits are tabular
  int lx = x - (H_CARD_X[i] + (H_CARD_W - 2 * adv) / 2);
  int ly = y - (H_CARD_Y + (H_CARD_H - BIG_CAP_H) / 2);
  int k = lx < adv ? 0 : 1;
  int8_t v = d[2 * i + k];
  float a = glyphAlpha(BIG_GLYPHS, BIG_DATA, v < 0 ? BIG_DASH : v, lx - k * adv, ly);
  if (a > 0) c = mix(c, H_WHITE, a);
  if (shade != 1.0f) c = scale(c, shade);
  return mix(under, c, clamp01(0.5f - s));
}

static RGB colonLayer(int x, int y, RGB under) {
  if (abs(x - H_COLON_X) > 12) return under;
  int cy = H_CARD_Y + H_CARD_H / 2;
  for (int k = -1; k <= 1; k += 2) {
    float dx = x + 0.5f - H_COLON_X, dy = y + 0.5f - (cy + k * H_COLON_DY);
    float a = clamp01(H_COLON_R + 0.5f - sqrtf(dx * dx + dy * dy));
    if (a > 0) return mix(under, H_WHITE, a);
  }
  return under;
}

static void drawCard(int i, const int8_t* d) {
  renderRect(H_CARD_X[i] - 1, H_CARD_Y - 1, H_CARD_W + 2, H_CARD_H + 2,
             [&](int x, int y) { return cardLayer(i, d, x, y, hBackground(x, y), 1.0f); });
}

// One frame of card i turning from digits o to n. phase 0: old top half folds down onto the hinge;
// phase 1: new bottom half unfolds from it. t = 0..1 within the phase.
static void flipFrame(int i, const int8_t* o, const int8_t* n, int phase, float t) {
  renderRect(H_CARD_X[i] - 1, H_CARD_Y - 1, H_CARD_W + 2, H_CARD_H + 2, [&](int x, int y) {
    RGB under = hBackground(x, y);
    int ly = y - H_CARD_Y;
    RGB c;
    if (phase == 0) {
      float h = H_HALF * (1.0f - t);
      if (ly < H_HALF && ly >= H_HALF - h && h >= 1.0f)
        c = cardLayer(i, o, x, H_CARD_Y + H_HALF - (int)((H_HALF - ly) * H_HALF / h), under, 1.0f - 0.5f * t);
      else c = cardLayer(i, ly < H_HALF ? n : o, x, y, under, 1.0f);
    } else {
      float h = H_HALF * t;
      int dy = ly - H_HALF;
      if (ly >= H_HALF && dy < h && h >= 1.0f)
        c = cardLayer(i, n, x, H_CARD_Y + H_HALF + (int)(dy * H_HALF / h), under, 0.5f + 0.5f * t);
      else c = cardLayer(i, ly < H_HALF ? n : o, x, y, under, 1.0f);
    }
    if ((ly == H_HALF || ly == H_HALF - 1) && x >= H_CARD_X[i] && x < H_CARD_X[i] + H_CARD_W) c = scale(c, 0.25f);
    return c;
  });
}

// Non-blocking flip: one frame per uiRender() call.
static struct { bool active; bool ch[2]; int8_t from[4], to[4]; int phase, frame; } flip;

static void flipStep() {
  flip.frame++;
  if (flip.frame > H_FLIP_FRAMES) { flip.frame = 1; flip.phase++; }
  if (flip.phase > 1) {
    for (int i = 0; i < 2; i++) if (flip.ch[i]) drawCard(i, flip.to);
    flip.active = false;
    return;
  }
  float t = (float)flip.frame / H_FLIP_FRAMES;
  t = t * t * (3 - 2 * t);
  for (int i = 0; i < 2; i++) if (flip.ch[i]) flipFrame(i, flip.from, flip.to, flip.phase, t);
}

// ---- bottom row ----
enum PillStyle : uint8_t { PILL_PINK, PILL_GREY, PILL_ORANGE };
struct HomeBtn { int x, w; RGB fill, border, ink; const char* label; uint8_t action; };
struct Bottom {
  char pillText[6];               // "HH:MM" or "--:--"
  PillStyle pillStyle;
  int pillW;
  float textX, iconX;
  HomeBtn btn[2];
  int nBtn;
  float progress;                 // 0..1, < 0 = none
  bool progressRed;
  char status[120];
  RGB statusInk;
};

static int smallIdx(char ch) { return ch == ':' ? SMALL_COLON : ch == '-' ? SMALL_DASH : ch - '0'; }

static RGB pillColour(PillStyle s, float u) {
  switch (s) {
    case PILL_GREY:   return mix(RGB{150, 150, 150}, RGB{118, 118, 118}, u);
    case PILL_ORANGE: return mix(RGB{255, 178, 80}, RGB{255, 118, 58}, u);
    default:          return mix(RGB{255, 96, 124}, RGB{240, 56, 160}, u);
  }
}

static RGB bottomPixel(const Bottom& b, int x, int y) {
  RGB c = hBackground(x, y);
  float fx = x + 0.5f, fy = y + 0.5f;

  // wake pill
  if (x <= H_PILL_X + b.pillW + 1 && y >= H_ROW_Y - 1 && y <= H_ROW_Y + H_ROW_H) {
    float d = sdRoundRect(fx, fy, H_PILL_X, H_ROW_Y, b.pillW, H_ROW_H, H_PILL_R);
    if (d <= 0.5f) {
      RGB p = pillColour(b.pillStyle, clamp01((float)(x - H_PILL_X) / b.pillW));
      p = mix(p, H_WHITE, 0.10f * clamp01(1.0f - (float)(y - H_ROW_Y) / (H_ROW_H * 0.5f)));   // soft top highlight
      int ty = H_ROW_Y + (H_ROW_H - SMALL_CAP_H) / 2, px = (int)b.textX;
      for (const char* s = b.pillText; *s; s++) {
        int gi = smallIdx(*s), adv = SMALL_GLYPHS[gi].adv;
        if (x >= px && x < px + adv) {
          float a = glyphAlpha(SMALL_GLYPHS, SMALL_DATA, gi, x - px, y - ty);
          if (a > 0) p = mix(p, H_PILL_FG, a);
          break;
        }
        px += adv;
      }
      float cx = b.iconX, cy = H_ROW_Y + H_ROW_H * 0.5f;                     // clock icon
      float dr = fabsf(sqrtf((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy)) - H_ICON_R);
      float dh = fminf(sdSegment(fx, fy, cx, cy, cx, cy - 5.0f), sdSegment(fx, fy, cx, cy, cx + 3.5f, cy + 2.5f));
      float a = clamp01(1.5f - fminf(dr, dh));
      if (a > 0) p = mix(p, H_PILL_FG, a);
      return mix(c, p, clamp01(0.5f - d));
    }
  }

  // buttons (labels are drawn on top afterwards with the TFT font)
  for (int i = 0; i < b.nBtn; i++) {
    const HomeBtn& k = b.btn[i];
    if (x < k.x - 1 || x > k.x + k.w || y < H_ROW_Y - 1 || y > H_ROW_Y + H_ROW_H) continue;
    float d = sdRoundRect(fx, fy, k.x, H_ROW_Y, k.w, H_ROW_H, H_PILL_R);
    if (d > 0.5f) continue;
    RGB f = mix(k.fill, k.border, clamp01(1.0f - fabsf(d + 0.5f)));
    return mix(c, f, clamp01(0.5f - d));
  }

  // progress bar between the pill and the buttons
  if (b.progress >= 0) {
    int x0 = H_PILL_X + b.pillW + 16, x1 = (b.nBtn ? b.btn[b.nBtn - 1].x : 468) - 16;
    float cy = H_ROW_Y + H_ROW_H * 0.5f;
    if (x1 - x0 > 40 && fabsf(fy - cy) < 6) {
      float d = sdRoundRect(fx, fy, x0, cy - 4, x1 - x0, 8, 4);
      if (d <= 0.5f) {
        RGB f = (fx - x0) <= b.progress * (x1 - x0) ? (b.progressRed ? H_RED : H_GREEN) : H_TRACK;
        return mix(c, f, clamp01(0.5f - d));
      }
    }
  }
  return c;
}

static void drawBottom(const Bottom& b) {
  renderRect(0, H_BOTTOM_Y, 480, 320 - H_BOTTOM_Y, [&](int x, int y) { return bottomPixel(b, x, y); });
  tft.setTextDatum(MC_DATUM);
  for (int i = 0; i < b.nBtn; i++) {
    const HomeBtn& k = b.btn[i];
    tft.setTextColor(rgb565(k.ink));                              // no background = drawn over the button
    tft.drawString(k.label, k.x + k.w / 2, H_ROW_Y + H_ROW_H / 2 + 1, 4);
  }
  if (b.status[0]) {
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(rgb565(b.statusInk));
    tft.drawString(b.status, 14, H_STATUS_Y, 2);
  }
}

static void fmtTime(time_t t, char* out, size_t n) {
  if (!t) { snprintf(out, n, "--:--"); return; }
  struct tm lt;
  localtime_r(&t, &lt);
  snprintf(out, n, "%s %02d:%02d", DAYS[lt.tm_wday], lt.tm_hour, lt.tm_min);
}

static void buildBottom(const UiModel& m, Bottom& b) {
  memset(&b, 0, sizeof(b));
  // pill: next wake
  if (m.wake) { struct tm lt; localtime_r(&m.wake, &lt); snprintf(b.pillText, sizeof(b.pillText), "%02d:%02d", lt.tm_hour, lt.tm_min); }
  else strcpy(b.pillText, "--:--");
  b.pillStyle = m.alarmFlash ? PILL_ORANGE : (m.ringDay ? PILL_PINK : PILL_GREY);
  int tw = 0;
  for (const char* s = b.pillText; *s; s++) tw += SMALL_GLYPHS[smallIdx(*s)].adv;
  b.textX = H_PILL_X + H_PILL_PAD;
  b.iconX = b.textX + tw + 7 + H_ICON_R;
  b.pillW = (int)(b.iconX + H_ICON_R + 1 + H_PILL_PAD) - H_PILL_X;

  // buttons, right-aligned: SET, then the data-mode label button to its left
  const RGB ink = H_WHITE;
  int right = 468;
  auto add = [&](const char* label, uint8_t action, RGB fill, RGB border, RGB inkc) {
    int w = tft.textWidth(label, 4) + 32;
    right -= w;
    b.btn[b.nBtn++] = {right, w, fill, border, inkc, label, action};
    right -= 10;
  };
  add("SET", UA_OPEN_SETTINGS, H_CARD, H_BORDER, ink);
  if (m.dataMode) {
    if (m.state == ST_ALARM || m.probe) add("I'M AWAKE", UA_AWAKE, H_GREEN, H_GREEN, RGB{0, 0, 0});
    else if (m.state == ST_UP) add("BACK IN BED", UA_BACK_IN_BED, H_CARD, H_BORDER, ink);
    else if (m.state == ST_IDLE || m.state == ST_NAG || m.state == ST_LOCKED || m.state == ST_SLEEPING)
      add("BED", UA_BED, H_CARD, H_BORDER, ink);
  }

  // progress: early-unlock hold (red) or minutes up (green)
  b.progress = m.holdFrac >= 0 ? m.holdFrac : (m.state == ST_UP ? m.upMin / UP_REQUIRED_MIN : -1);
  if (b.progress > 1) b.progress = 1;
  b.progressRed = m.holdFrac >= 0;

  // status line (empty when idle: the pill already shows the next wake)
  char w[16];
  fmtTime(m.wake, w, sizeof(w));
  const char* title = "";
  char t[64] = "";
  b.statusInk = RGB{200, 200, 200};
  if (m.probe) { snprintf(t, sizeof(t), "Still awake? Tap I'M AWAKE  (%d s)", m.probeLeftS); b.statusInk = RGB{120, 165, 245}; }
  else switch (m.state) {
    case ST_IDLE:     if (!m.now) title = "Waiting for the time..."; break;
    case ST_NAG:      title = "Put your phone in!"; b.statusInk = H_RED; break;
    case ST_LOCKED:   snprintf(t, sizeof(t), "Locked until %s", w); break;
    case ST_SLEEPING: snprintf(t, sizeof(t), "Good night - wake %s", w); break;
    case ST_ALARM:    title = m.awaitingAwakeTap ? "WAKE UP - tap I'M AWAKE" : "WAKE UP - get out of bed"; b.statusInk = RGB{255, 170, 80}; break;
    case ST_UP:       snprintf(t, sizeof(t), "Up %d / %d min", (int)m.upMin, UP_REQUIRED_MIN); b.statusInk = H_GREEN; break;
    case ST_UNLOCKED: title = "Unlocked - take your phone"; break;
    default: break;
  }
  if (!t[0]) snprintf(t, sizeof(t), "%s", title);
  const char* tag = m.dataMode ? "DATA MODE" : (m.aiTrained ? "" : "AI untrained");
  if (m.message) snprintf(b.status, sizeof(b.status), "%s%s%s", t, t[0] ? "  |  " : "", m.message);
  else snprintf(b.status, sizeof(b.status), "%s%s%s", t, (t[0] && tag[0]) ? "  |  " : "", tag);
}

// Touch targets for the home screen = the bottom-row buttons.
static void registerButtons(const Bottom& b) {
  nBtns = 0;
  for (int i = 0; i < b.nBtn; i++) addBtn(b.btn[i].x, H_ROW_Y, b.btn[i].w, H_ROW_H, b.btn[i].action);
}

// ---- top bar: date + link dots, small and muted over the background ----
static void drawTopBar(const UiModel& m) {
  renderRect(0, 0, 480, H_CARD_Y - 2, [](int x, int y) { return hBackground(x, y); });
  char top[32];
  struct tm lt;
  if (m.now) { localtime_r(&m.now, &lt); snprintf(top, sizeof(top), "%s %d %s", DAYS[lt.tm_wday], lt.tm_mday, MONTHS[lt.tm_mon]); }
  else snprintf(top, sizeof(top), "time unknown");
  uint16_t muted = tft.color565(130, 130, 130);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(muted);
  tft.drawString(top, 14, 7, 2);
  const char* names[3] = {"WIFI", "NODE", "CLOUD"};
  bool ok[3] = {m.wifi, m.node, m.cloud};
  for (int i = 0; i < 3; i++) {
    int x = 284 + i * 66;
    tft.fillCircle(x, 15, 4, ok[i] ? C_GREEN : C_RED);
    tft.setTextColor(muted);
    tft.drawString(names[i], x + 9, 7, 2);
  }
}

static char lastTop[64] = "", lastBottom[220] = "";

static void drawHome(const UiModel& m, bool force) {
  if (force) {
    renderRect(0, 0, 480, 320, [](int x, int y) { return colonLayer(x, y, hBackground(x, y)); });
    lastTop[0] = lastBottom[0] = 0;
    flip.active = false;
    for (int i = 0; i < 4; i++) shownD[i] = -2;
  }

  // top bar
  char sig[64];
  struct tm lt;
  if (m.now) localtime_r(&m.now, &lt);
  snprintf(sig, sizeof(sig), "%d/%d|%d%d%d", m.now ? lt.tm_yday : -1, m.now ? lt.tm_year : -1, m.wifi, m.node, m.cloud);
  if (strcmp(sig, lastTop) != 0) { strcpy(lastTop, sig); drawTopBar(m); }

  // cards: draw directly on a full redraw, flip when a digit changes
  int8_t d[4] = {-1, -1, -1, -1};
  if (m.now) { d[0] = lt.tm_hour / 10; d[1] = lt.tm_hour % 10; d[2] = lt.tm_min / 10; d[3] = lt.tm_min % 10; }
  if (flip.active) flipStep();
  else if (memcmp(d, shownD, 4) != 0) {
    bool first = shownD[0] == -2;
    flip.ch[0] = d[0] != shownD[0] || d[1] != shownD[1];
    flip.ch[1] = d[2] != shownD[2] || d[3] != shownD[3];
    memcpy(flip.from, shownD, 4);
    memcpy(flip.to, d, 4);
    memcpy(shownD, d, 4);
    if (first) { drawCard(0, d); drawCard(1, d); }
    else { flip.active = true; flip.phase = 0; flip.frame = 0; flipStep(); }
  }

  // bottom row + status line
  Bottom b;
  buildBottom(m, b);
  char s[220];
  snprintf(s, sizeof(s), "%s|%d|%d|%d|%d|%s", b.pillText, b.pillStyle, b.nBtn ? b.btn[b.nBtn - 1].action : 0,
           b.nBtn, b.progress < 0 ? -1 : (int)(b.progress * 60), b.status);
  if (strcmp(s, lastBottom) != 0) {
    strcpy(lastBottom, s);
    drawBottom(b);
    registerButtons(b);
  }
}

static void drawLockPrompt(const UiModel& m) {
  tft.fillScreen(C_BG);
  nBtns = 0;
  char w[16], line[64];
  fmtTime(m.wake, w, sizeof(w));
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_ORANGE, C_BG);
  tft.drawString("Phone inserted", 240, 70, 4);
  tft.setTextColor(C_TEXT, C_BG);
  snprintf(line, sizeof(line), "Lock it until %s ?", w);
  tft.drawString(line, 240, 120, 4);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString("It is before bedtime. The button also ejects.", 240, 160, 2);
  button(40, 220, 180, 60, "LOCK", C_ORANGE, TFT_BLACK, UA_LOCK_NOW);
  button(260, 220, 180, 60, "EJECT", C_CARD, C_TEXT, UA_EJECT);
}

void uiRender(const UiModel& m) {
  bool force = fullRedraw;
  fullRedraw = false;
  switch (screen) {
    case SCR_SETTINGS: if (force) drawSettings(false); break;
    case SCR_SETUP:    if (force) drawSettings(true); break;
    case SCR_LOCK_PROMPT: if (force) drawLockPrompt(m); break;
    default:           drawHome(m, force); break;
  }
}
