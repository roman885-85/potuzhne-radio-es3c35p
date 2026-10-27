#include "m2ui.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "../core/options.h"
#include "../displays/dspcore.h"
#include "../displays/tools/spidma.h"
#include "../displays/tools/lcdout.h"
#include "m2simd.h"
#include "m2pages.h"
#include "m2lang.h"
#include "../extras/yoExtras.h"

extern DspCore dsp;

volatile uint32_t g_m2Frames = 0;

namespace m2 {

Menu M;

/*  ---------- калібрування сенсора ----------
    Чотири позначки по кутах (за 24 пікселі від краю). Дотики збираються
    «сирими» координатами; далі — перевірка й лінійна поправка по кожній осі.  */
static const int16_t CAL_TX[4] = { TS_CAL_LO, TS_CAL_XHI, TS_CAL_XHI, TS_CAL_LO };   /* ЛВ, ПВ, ПН, ЛН */
static const int16_t CAL_TY[4] = { TS_CAL_LO, TS_CAL_LO,  TS_CAL_YHI, TS_CAL_YHI };
struct CalibState {
  bool     active = false;
  uint8_t  step = 0;               /* скільки позначок уже торкнулись (0..4) */
  int16_t  mx[4] = {0}, my[4] = {0};
  uint32_t lastT = 0;
  uint8_t  err = 0;                /* показати підказку про невдачу */
} s_cal;

/*  з m2menu.cpp: стан мережі й час для шапки  */
int8_t  bridgeRssi();
bool    bridgeClock(char* out, uint8_t cap, uint8_t& minute);
void    bridgeClosed();                 /* меню закрилось: плеєр — наново */
void    bridgeFade(uint16_t level);     /* підсвітка під час затемнення */
uint16_t bridgeBright();

/*  Буфер смуг: на ES3C28P 320×32 (дві половини по 16 рядків), на ES3C35P
    480×16 — половини по 8 рядків: смугу ще й треба повернути у свій буфер
    (lcdout), а внутрішньої пам'яті обмаль.  */
static const int32_t STRIP_PX = (int32_t)PSW * (LCD_PUSH_COPIES ? 16 : 32);   /* у пікселях панелі */
static const uint32_t T_TRANS = 240;           /* перехід між сторінками, мс */

static inline float easeOut(float t){ if(t < 0) t = 0; if(t > 1) t = 1; float u = 1 - t; return 1 - u * u * u; }

/*  =================== Page =================== */
void Page::back(){ M.pop(); }

/*  =================== спільні елементи =================== */
void drawCard(Gfx& g, int16_t x, int16_t y, int16_t w, int16_t h, bool first, bool last, uint16_t c){
  if(first && last){ g.box(x, y, w, h, R_CARD, c); return; }
  if(!first && !last){ g.fill(x, y, w, h, c); return; }
  Rect s = g.narrow(x, y, w, h);
  if(first) g.box(x, y, w, h + R_CARD, R_CARD, c);
  else      g.box(x, y - R_CARD, w, h + R_CARD, R_CARD, c);
  g.restore(s);
}

void drawSwitch(Gfx& g, int16_t x, int16_t y, float pos, bool enabled){
  if(pos < 0) pos = 0; if(pos > 1) pos = 1;
  uint16_t track = Gfx::blend(C_SURF2, C_ACC, (uint8_t)(pos * 255));
  if(!enabled) track = Gfx::blend(track, C_SURF, 140);
  g.box(x, y, 40, 24, 12, track);
  g.circle(x + 12 + pos * 16, y + 12, 9.2f, enabled ? C_KNOB : C_TXT3);
}

void drawSeg(Gfx& g, int16_t x, int16_t y, int16_t w, int16_t h, const char* const* opts, uint8_t n, float pos, bool enabled){
  if(!n) return;
  g.box(x, y, w, h, 9, C_SURF2);
  const float pw = (w - 4) / (float)n;
  if(pos >= 0){
    int16_t px = (int16_t)lroundf(x + 2 + pos * pw);
    g.box(px, y + 2, (int16_t)pw, h - 4, 7, enabled ? C_ACC : C_TXT3);
  }
  for(uint8_t i = 0; i < n; i++){
    bool on = pos >= 0 && fabsf(pos - i) < 0.5f;
    int16_t cx = (int16_t)(x + 2 + pw * i + pw / 2);
    g.text(cx, y + h / 2 + 4, opts[i], F_SMB, on ? C_ACCTXT : (enabled ? C_TXT : C_TXT3), AL_C, (int16_t)pw - 4);
  }
}

void drawSlider(Gfx& g, int16_t x, int16_t y, int16_t w, float frac, bool enabled, float zero){
  if(frac < 0) frac = 0; if(frac > 1) frac = 1;
  const float kx = x + 9 + frac * (w - 18);
  g.box(x, y - 2, w, 4, 2, C_SURF2);
  uint16_t ac = enabled ? C_ACC : C_TXT3;
  if(zero >= 0){
    const float zx = x + 9 + zero * (w - 18);
    float a = zx < kx ? zx : kx, b = zx < kx ? kx : zx;
    if(b - a >= 1) g.fill((int16_t)a, y - 2, (int16_t)(b - a), 4, ac);
    g.fill((int16_t)zx - 1, y - 6, 2, 12, C_TXT3);
  }else if(kx - x >= 3) g.box(x, y - 2, (int16_t)(kx - x) + 2, 4, 2, ac);
  g.circle(kx, y, 9, enabled ? C_KNOB : C_TXT3);
}

void drawBadge(Gfx& g, int16_t x, int16_t y, uint8_t ic, uint16_t color){
  g.box(x, y, 28, 28, R_BADGE, color);
  /*  на світлих плашках (жовта) — темний значок  */
  uint16_t r = (color >> 11) & 31, gg = (color >> 5) & 63, b = color & 31;
  bool light = (r * 2 + gg * 3 + b) > 190;
  icon(g, ic, x + 14, y + 14, light ? C_ACCTXT : 0xFFFF, color);
}

/*  =================== рядки =================== */
Item iSection(const char* label){ Item i; i.type = IT_SECTION; i.label = label; return i; }
Item iNav(const char* label, uint8_t ic, uint16_t badge, TextFn value, ActFn act){ Item i; i.type = IT_NAV; i.label = label; i.icon = ic; i.badge = badge; i.text = value; i.act = act; return i; }
Item iSwitch(const char* label, uint8_t ic, uint16_t badge, GetFn get, SetFn set){ Item i; i.type = IT_SWITCH; i.label = label; i.icon = ic; i.badge = badge; i.get = get; i.set = set; return i; }
Item iSlider(const char* label, int16_t lo, int16_t hi, GetFn get, SetFn set, const char* unit){ Item i; i.type = IT_SLIDER; i.label = label; i.lo = lo; i.hi = hi; i.get = get; i.set = set; i.unit = unit; return i; }
Item iSliderPlay(const char* label, int16_t lo, int16_t hi, GetFn get, SetFn set, const char* unit, ActFn play){ Item i = iSlider(label, lo, hi, get, set, unit); i.act = play; return i; }
/*  кнопка ▶ повзунка: праворуч у рядку підпису; дотик ловимо ширше за саму кнопку  */
static const int16_t PLAY_W = 34, PLAY_H = 24, PLAY_ZONE = 56, PLAY_LINE = 34;
static Rect playRect(const Item& it){ return Rect(MX + CWID - 12 - PLAY_W, it.y + 6, PLAY_W, PLAY_H); }
static bool onPlay(const Item& it, int16_t x, int16_t y){ return it.act && y < it.y + PLAY_LINE && x >= MX + CWID - PLAY_ZONE; }
Item iSeg(const char* label, const char* const* opts, uint8_t n, GetFn get, SetFn set){ Item i; i.type = IT_SEG; i.label = label; i.opts = opts; i.nopts = n; i.get = get; i.set = set; return i; }
Item iButton(const char* label, uint8_t ic, ActFn act, uint16_t color){ Item i; i.type = IT_BUTTON; i.label = label; i.icon = ic; i.act = act; i.color = color; return i; }
Item iInfo(const char* label, TextFn value){ Item i; i.type = IT_INFO; i.label = label; i.text = value; return i; }
Item iNote(TextFn text, int16_t h){ Item i; i.type = IT_NOTE; i.text = text; i.h = h; return i; }
Item iGap(int16_t h){ Item i; i.type = IT_GAP; i.h = h; return i; }
Item iCustom(int16_t h, void (*draw)(Gfx&, int16_t, int16_t, int16_t, int16_t)){ Item i; i.type = IT_CUSTOM; i.h = h; i.cdraw = draw; return i; }

static bool isCard(uint8_t t){ return t == IT_NAV || t == IT_SWITCH || t == IT_SLIDER || t == IT_SEG || t == IT_BUTTON || t == IT_INFO; }
/*  власний рядок усередині картки (color == 1): тло картки малює список  */
static bool isCardItem(const Item& it){ return isCard(it.type) || (it.type == IT_CUSTOM && it.color == 1); }

void ListPage::enter(){
  for(uint8_t i = 0; i < _n; i++){ _it[i].ai = false; _it[i].sig = 0; }
  _layout();
}

void ListPage::_layout(){
  int16_t y = 4;
  for(uint8_t i = 0; i < _n; i++){
    Item& it = _it[i];
    it.vis = !it.show || it.show();
    if(!it.vis){ it.hh = 0; continue; }
    switch(it.type){
      case IT_SECTION: it.hh = (y <= 4) ? 22 : 30; break;
      case IT_GAP:     it.hh = it.h ? it.h : 8; break;
      case IT_NOTE:    it.hh = it.h ? it.h : 22; break;
      case IT_SLIDER:  it.hh = 58; break;
      case IT_SEG:     it.hh = (it.label && it.label[0]) ? 66 : 46; break;
      case IT_CUSTOM:  it.hh = it.h; break;
      default:         it.hh = 44; break;
    }
    it.y = y; y += it.hh;
  }
  /*  картка — підряд рядки-картки; перший і останній мають заокруглені кути  */
  int prev = -1;
  for(uint8_t i = 0; i < _n; i++){
    Item& it = _it[i];
    if(!it.vis) continue;
    bool card = isCardItem(it);
    it.first = card && (prev < 0 || !isCardItem(_it[prev]));
    it.last = card;
    if(card && prev >= 0 && isCardItem(_it[prev])) _it[prev].last = false;
    prev = i;
  }
  _h = y + 12;
  if(_h < CH) _h = CH;
}

uint32_t ListPage::_sigOf(Item& it){
  uint32_t h = 2166136261UL;
  auto mix = [&](uint32_t v){ h = (h ^ v) * 16777619UL; };
  if(it.enabled) mix(it.enabled() ? 1 : 2);
  if(it.get) mix((uint32_t)it.get());
  if(it.text){ const char* s = it.text(); if(s) for(; *s; s++) mix((uint8_t)*s); }
  return h;
}

void ListPage::tick(uint32_t now){
  /*  раз на 150 мс: чи не змінились видимість і значення  */
  if(now - _sigT >= 150){
    _sigT = now;
    bool relay = false;
    for(uint8_t i = 0; i < _n; i++) if(_it[i].show && _it[i].show() != _it[i].vis) relay = true;
    if(relay){ _layout(); M.inval(Rect(0, 0, SW, _h > 1000 ? 1000 : _h + 200)); }
    for(uint8_t i = 0; i < _n; i++){
      Item& it = _it[i];
      if(!it.vis || it.type == IT_SECTION || it.type == IT_GAP) continue;
      uint32_t s = _sigOf(it);
      if(s != it.sig){ if(it.sig) M.inval(Rect(0, it.y, SW, it.hh)); it.sig = s; }
    }
  }
  /*  плавні перемикачі, плашки вибору, повзунки  */
  static uint32_t lastT = 0;
  float k = (now - lastT) / 16.0f * 0.32f; if(k > 1) k = 1; if(k <= 0) k = 0.32f;
  lastT = now;
  for(uint8_t i = 0; i < _n; i++){
    Item& it = _it[i];
    if(!it.vis || !it.get) continue;
    if(it.type != IT_SWITCH && it.type != IT_SEG && it.type != IT_SLIDER) continue;
    float target = it.type == IT_SWITCH ? (it.get() ? 1.0f : 0.0f) : (float)it.get();
    if(it.type == IT_SLIDER && _dragId == i && now < _dragHold) target = _dragV;
    if(!it.ai){ it.anim = target; it.ai = true; continue; }
    float d = target - it.anim;
    if(fabsf(d) < 0.001f) continue;
    float lim = it.type == IT_SLIDER ? 0.5f : 0.02f;
    it.anim = fabsf(d) < lim ? target : it.anim + d * k;
    M.inval(Rect(0, it.y, SW, it.hh));
  }
}

void ListPage::draw(Gfx& g){
  for(uint8_t i = 0; i < _n; i++){
    Item& it = _it[i];
    if(!it.vis || !it.hh) continue;
    if(!g.visible(0, it.y, SW, it.hh)) continue;
    _drawItem(g, it);
  }
}

void ListPage::_drawItem(Gfx& g, Item& it){
  const bool en = !it.enabled || it.enabled();
  const int16_t x = MX, w = CWID, y = it.y, h = it.hh;
  uint16_t tc = en ? C_TXT : C_TXT3;
  switch(it.type){
    case IT_SECTION:
      if(it.label) g.text(x + 12, y + h - 9, it.label, F_SMB, C_TXT2);
      return;
    case IT_GAP: return;
    case IT_NOTE: {
      if(!it.text) return;
      /*  Переклад беремо для всього напису, а вже потім ріжемо на рядки:
          сам рядок із «\n» до Gfx::text не доходить (m2lang).  */
      const char* s = tr(it.text());
      if(!s) return;
      char line[96]; int16_t by = y + 15;
      while(*s){
        const char* e = strchr(s, '\n');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if(n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, s, n); line[n] = 0;
        g.text(x + 12, by, line, F_SM, C_TXT2, AL_L, w - 24);
        by += 14;
        if(!e) break;
        s = e + 1;
      }
      return;
    }
    case IT_CUSTOM:
      if(it.color == 1) drawCard(g, x, y, w, h, it.first, it.last);
      if(it.cdraw) it.cdraw(g, x, y, w, h);
      return;
    default: break;
  }
  drawCard(g, x, y, w, h, it.first, it.last);
  int16_t lx = x + 14;
  if(!it.first) g.fill(x + (it.icon ? 50 : 14), y, w - (it.icon ? 50 : 14), 1, C_LINE);
  if(it.icon && it.type != IT_BUTTON){
    drawBadge(g, x + 12, y + (h - 28) / 2, it.icon, en ? it.badge : C_TXT3);
    lx = x + 50;
  }
  switch(it.type){
    case IT_NAV: {
      int16_t vw = 0;
      icon(g, IC_CHEV, x + w - 16, y + h / 2, C_TXT2, C_SURF);
      if(it.text){
        const char* v = it.text();
        if(v && *v) vw = g.text(x + w - 28, y + h / 2 + 4, v, F_SM, C_TXT2, AL_R, 120);
      }
      g.text(lx, y + h / 2 + 4, it.label, F_ROW, tc, AL_L, x + w - 36 - vw - lx);
      break;
    }
    case IT_SWITCH:
      g.text(lx, y + h / 2 + 4, it.label, F_ROW, tc, AL_L, x + w - 64 - lx);
      drawSwitch(g, x + w - 52, y + (h - 24) / 2, !it.ai ? (it.get && it.get() ? 1 : 0) : it.anim, en);
      break;
    case IT_INFO: {
      const char* v = it.text ? it.text() : "";
      int16_t vw = g.text(x + w - 14, y + h / 2 + 4, v ? v : "", F_SM, C_TXT2, AL_R, 170);
      g.text(lx, y + h / 2 + 4, it.label, F_ROW, tc, AL_L, x + w - 22 - vw - lx);
      break;
    }
    case IT_BUTTON: {
      uint16_t c = it.color ? it.color : C_ACC;
      if(!en) c = C_TXT3;
      const char* l = (it.text && it.text()) ? it.text() : it.label;
      int16_t tw = Gfx::textW(l, F_ROWB);
      int16_t total = tw + (it.icon ? 26 : 0);
      int16_t sx = x + (w - total) / 2;
      if(it.icon){ icon(g, it.icon, sx + 10, y + h / 2, c, C_SURF); sx += 26; }
      g.text(sx, y + h / 2 + 4, l, F_ROWB, c, AL_L, w - 20);
      break;
    }
    case IT_SLIDER: {
      float v = !it.ai ? (float)(it.get ? it.get() : it.lo) : it.anim;
      char b[20];
      int32_t iv = (_dragId >= 0 && &_it[_dragId] == &it && millis() < _dragHold) ? (int32_t)_dragV : (it.get ? it.get() : 0);
      snprintf(b, sizeof(b), "%d%s", (int)iv, it.unit ? it.unit : "");
      int16_t vx = x + w - 14;
      if(it.act){
        Rect pr = playRect(it);
        g.box(pr.x, pr.y, pr.w, pr.h, pr.h / 2, en ? C_SURF2 : C_SURF);
        icon(g, IC_PLAY, pr.x + pr.w / 2 + 1, pr.y + pr.h / 2, en ? C_ACC : C_TXT2, C_SURF2);
        vx = pr.x - 8;
      }
      int16_t vw = g.text(vx, y + 22, b, F_ROWB, tc, AL_R);
      g.text(x + 14, y + 22, it.label, F_ROW, tc, AL_L, vx - vw - x - 26);
      float frac = (v - it.lo) / (float)(it.hi - it.lo);
      float zero = (it.lo < 0 && it.hi > 0) ? (0 - it.lo) / (float)(it.hi - it.lo) : -1;
      drawSlider(g, x + 12, y + 40, w - 24, frac, en, zero);
      break;
    }
    case IT_SEG: {
      bool hasL = it.label && it.label[0];
      if(hasL) g.text(x + 14, y + 21, it.label, F_ROW, tc, AL_L, w - 28);
      float pos = !it.ai ? (float)(it.get ? it.get() : -1) : it.anim;
      drawSeg(g, x + 8, y + (hasL ? 30 : 8), w - 16, 30, it.opts, it.nopts, pos, en);
      break;
    }
    default: break;
  }
}

int16_t ListPage::hit(int16_t x, int16_t y, Rect& r, uint8_t& radius){
  (void)x;
  for(uint8_t i = 0; i < _n; i++){
    Item& it = _it[i];
    if(!it.vis || !isCard(it.type) || it.type == IT_INFO) continue;
    if(y < it.y || y >= it.y + it.hh) continue;
    if(it.enabled && !it.enabled()) return -1;
    r = Rect(MX, it.y, CWID, it.hh);
    radius = (it.first || it.last) ? R_CARD : 0;
    /*  Неточний дотик теж спрацьовує: увесь рядок — і смуга вибору (варіант під
        пальцем по горизонталі), і повзунок.  */
    if(it.type == IT_SEG){
      int16_t sy = it.y + ((it.label && it.label[0]) ? 30 : 8);
      r = Rect(MX + 8, sy, CWID - 16, 30); radius = 9;
    }
    _onPlay = it.type == IT_SLIDER && onPlay(it, x, y);
    if(_onPlay){ r = playRect(it); radius = PLAY_H / 2; }
    return i;
  }
  return -1;
}

uint8_t ListPage::grab(int16_t id){
  if(id < 0 || id >= _n) return 0;
  return _it[id].type == IT_SLIDER && !_onPlay ? 1 : 0;   /* з кнопки ▶ повзунок не тягнемо */
}

int32_t ListPage::_sliderAt(const Item& it, int16_t x) const {
  const float x0 = MX + 12 + 9, w = CWID - 24 - 18;
  /*  краї липкі: біля кінця доріжки — одразу мінімум чи максимум  */
  float f = (x - x0 - 6) / (w - 12);
  if(f < 0) f = 0; if(f > 1) f = 1;
  int32_t v = it.lo + lroundf(f * (it.hi - it.lo));
  /*  двобічний: середина притягує  */
  if(it.lo < 0 && it.hi > 0){
    const float zx = x0 + (0 - it.lo) / (float)(it.hi - it.lo) * w;
    if(fabsf(x - zx) <= 6) v = 0;
  }
  return v;
}

void ListPage::drag(int16_t id, int16_t x, int16_t y, bool end){
  (void)y;
  if(id < 0 || id >= _n || _it[id].type != IT_SLIDER) return;
  int32_t v = _sliderAt(_it[id], x);
  _dragId = id; _dragV = (float)v; _dragHold = millis() + (end ? 600 : 100000);
  M.inval(Rect(0, _it[id].y, SW, _it[id].hh));
  M.postValue(id, v, end);
}

void ListPage::tap(int16_t id, int16_t x, int16_t y){
  (void)y;
  if(id < 0 || id >= _n) return;
  Item& it = _it[id];
  if(it.enabled && !it.enabled()) return;
  switch(it.type){
    case IT_NAV: case IT_BUTTON: if(it.act) it.act(); break;
    case IT_SWITCH: if(it.get && it.set) it.set(it.get() ? 0 : 1); break;
    case IT_SEG: {
      if(!it.set || !it.nopts) break;
      const int16_t x0 = MX + 8, w = CWID - 16;
      int i = (x - x0) * it.nopts / w;
      if(i < 0) i = 0; if(i >= it.nopts) i = it.nopts - 1;
      it.set(i);
      break;
    }
    case IT_SLIDER:
      if(onPlay(it, x, y)){ it.act(); break; }
      if(it.set) it.set(_sliderAt(it, x));
      break;
    default: break;
  }
}

void ListPage::value(int16_t id, int32_t v){
  if(id < 0 || id >= _n) return;
  if(_it[id].type == IT_SLIDER && _it[id].set) _it[id].set(v);
}

/*  Екран маленький, палець великий: не влучив — шукаємо найближчий елемент
    довкола, кільцями до 18 пікселів. Діє на всіх сторінках, без їхньої участі.  */
static int16_t hitNear(Page* p, int16_t x, int16_t y, Rect& r, uint8_t& rad){
  static const int8_t OFF[][2] = {
    { 0, 6 }, { 0, -6 }, { -6, 0 }, { 6, 0 }, { -6, 6 }, { 6, 6 }, { -6, -6 }, { 6, -6 },
    { 0, 12 }, { 0, -12 }, { -12, 0 }, { 12, 0 }, { -9, 9 }, { 9, 9 }, { -9, -9 }, { 9, -9 },
    { 0, 18 }, { 0, -18 }, { -18, 0 }, { 18, 0 }, { -13, 13 }, { 13, 13 }, { -13, -13 }, { 13, -13 } };
  int16_t id = p->hit(x, y, r, rad);
  if(id >= 0) return id;
  for(uint8_t i = 0; i < sizeof(OFF) / sizeof(OFF[0]); i++){
    id = p->hit(x + OFF[i][0], y + OFF[i][1], r, rad);
    if(id >= 0) return id;
  }
  return -1;
}

/*  =================== меню: черги =================== */
enum : uint8_t { C_OPEN = 1, C_PUSH, C_POP, C_POPTO, C_REPLACE, C_CLOSE };
enum : uint8_t { T_PRESS = 1, T_DRAG, T_RELEASE };
enum : uint8_t { A_TAP = 1, A_HOLD, A_BACK, A_VALUE };

void Menu::_cmdPush(uint8_t op, Page* p){
  portENTER_CRITICAL(&_mux);
  uint8_t n = (_cmdH + 1) % QN;
  if(n != _cmdT){ _cmd[_cmdH].op = op; _cmd[_cmdH].p = p; _cmdH = n; }
  portEXIT_CRITICAL(&_mux);
}

void Menu::_post(uint8_t kind, Page* p, int16_t id, int16_t x, int16_t y, int32_t v){
  portENTER_CRITICAL(&_mux);
  uint8_t n = (_aqH + 1) % 16;
  if(n != _aqT){ Act& a = _aq[_aqH]; a.kind = kind; a.p = p; a.id = id; a.x = x; a.y = y; a.v = v; _aqH = n; }
  portEXIT_CRITICAL(&_mux);
}

void Menu::postValue(int16_t id, int32_t v, bool final){
  uint32_t now = millis();
  if(!final && now - _valT < 80) return;
  _valT = now;
  _post(A_VALUE, top(), id, 0, 0, v);
}

/*  Логічний стек — у головному циклі: з ним працюють enter/leave. Задача
    дисплея отримує ті самі зміни чергою.  */
static Page* s_lst[10];
static uint8_t s_ld = 0;

Page* Menu::ltop() const { return s_ld ? s_lst[s_ld - 1] : nullptr; }

void Menu::open(Page* root){
  if(_open || _openReq || !root) return;
  while(s_ld) s_lst[--s_ld]->leave();
  root->scroll = 0; root->scrollP = 0;
  root->enter();
  s_lst[s_ld++] = root;
  _lastTouch = millis();
  _openReq = true;
  _cmdPush(C_OPEN, root);
}

void Menu::push(Page* p){
  if(!p || s_ld >= 10) return;
  p->scroll = 0; p->scrollP = 0;
  p->enter();
  s_lst[s_ld++] = p;
  _cmdPush(C_PUSH, p);
}

void Menu::pop(){
  if(s_ld <= 1){ close(); return; }
  s_lst[--s_ld]->leave();
  _cmdPush(C_POP, nullptr);
}

void Menu::popTo(Page* p){
  while(s_ld > 1 && s_lst[s_ld - 1] != p){ s_lst[--s_ld]->leave(); _cmdPush(C_POP, nullptr); }
}

void Menu::replace(Page* p){
  if(!p) return;
  if(s_ld) s_lst[s_ld - 1]->leave();
  else s_ld = 1;
  p->scroll = 0; p->scrollP = 0;
  p->enter();
  s_lst[s_ld - 1] = p;
  _cmdPush(C_REPLACE, p);
}

void Menu::close(){
  if(!s_ld && !_open) return;
  _closeAt = millis() + 170;                     /* хвилю від дотику ще видно */
  _cmdPush(C_CLOSE, nullptr);
}

void Menu::closeNow(){
  if(!s_ld && !_open) return;
  _closeAt = millis();
  _cmdPush(C_CLOSE, nullptr);
}

void Menu::onPress(uint16_t x, uint16_t y){
  portENTER_CRITICAL(&_mux);
  uint8_t n = (_tqH + 1) % 24;
  if(n != _tqT){ _tq[_tqH] = { T_PRESS, (int16_t)x, (int16_t)y, millis() }; _tqH = n; }
  portEXIT_CRITICAL(&_mux);
}
void Menu::onDrag(uint16_t x, uint16_t y){
  portENTER_CRITICAL(&_mux);
  /*  кілька рухів підряд — достатньо останнього  */
  uint8_t prev = (_tqH + 23) % 24;
  if(_tqH != _tqT && _tq[prev].kind == T_DRAG){ _tq[prev].x = x; _tq[prev].y = y; _tq[prev].t = millis(); }
  else{
    uint8_t n = (_tqH + 1) % 24;
    if(n != _tqT){ _tq[_tqH] = { T_DRAG, (int16_t)x, (int16_t)y, millis() }; _tqH = n; }
  }
  portEXIT_CRITICAL(&_mux);
}
void Menu::onRelease(uint16_t x, uint16_t y){
  portENTER_CRITICAL(&_mux);
  uint8_t n = (_tqH + 1) % 24;
  if(n != _tqT){ _tq[_tqH] = { T_RELEASE, (int16_t)x, (int16_t)y, millis() }; _tqH = n; }
  portEXIT_CRITICAL(&_mux);
}

/*  =================== головний цикл =================== */
void Menu::loop(){
  if(s_cal.active){
    if(millis() - s_cal.lastT > 45000UL){ s_cal.active = false; toast("Калібрування скасовано"); invalAll(); }
    _lastTouch = millis();                        /* меню не закривати під час калібрування */
    return;
  }
  stationsPoll();
  /*  меню закрилось (задача дисплея вже погасила й намалювала плеєр) — прибираємо сторінки  */
  if(s_ld && !_open && !_openReq){
    while(s_ld) s_lst[--s_ld]->leave();
    portENTER_CRITICAL(&_mux);
    _aqT = _aqH;
    portEXIT_CRITICAL(&_mux);
    return;
  }
  if(!s_ld && !_open) return;
  uint32_t now = millis();
  for(;;){
    Act a;
    portENTER_CRITICAL(&_mux);
    bool any = _aqT != _aqH;
    if(any){ a = _aq[_aqT]; _aqT = (_aqT + 1) % 16; }
    portEXIT_CRITICAL(&_mux);
    if(!any) break;
    if(!a.p) continue;
    bool live = false;
    for(uint8_t i = 0; i < s_ld; i++) if(s_lst[i] == a.p) live = true;
    if(!live) continue;                          /* сторінку вже закрили */
    switch(a.kind){
      case A_TAP:   a.p->tap(a.id, a.x, a.y); break;
      case A_HOLD:  if(!a.p->hold(a.id)) a.p->tap(a.id, a.x, a.y); break;
      case A_BACK:  if(a.p->canBack()) a.p->back(); break;
      case A_VALUE: a.p->value(a.id, a.v); break;
    }
  }
  if(s_ld) s_lst[s_ld - 1]->loop(now);
  /*  хвилину без дотиків — назад на плеєр  */
  if(s_ld && _open && !s_lst[s_ld - 1]->keepOpen() && now - _lastTouch > 60000UL){ _lastTouch = now; close(); }
}

/*  =================== задача дисплея =================== */
void Menu::_markRaw(int16_t x, int16_t y, int16_t w, int16_t h){
  if(w <= 0 || h <= 0) return;
  /*  логічний прямокутник → пікселі панелі (з запасом до цілих пікселів)  */
  int16_t x1 = uiPceil(x + w) - 1, y1 = uiPceil(y + h) - 1;
  x = uiPfloor(x); y = uiPfloor(y);
  if(x < 0) x = 0; if(y < 0) y = 0;
  if(x1 >= PSW) x1 = PSW - 1; if(y1 >= PSH) y1 = PSH - 1;
  if(x > x1 || y > y1) return;
  int tx0 = x / 16, tx1 = x1 / 16, ty0 = y / 16, ty1 = y1 / 16;
  uint32_t bits = (((1UL << (tx1 - tx0 + 1)) - 1) << tx0);
  portENTER_CRITICAL(&_mux);
  for(int ty = ty0; ty <= ty1; ty++) _dirty[ty] |= bits;
  portEXIT_CRITICAL(&_mux);
}
void Menu::invalScreen(int16_t x, int16_t y, int16_t w, int16_t h){ _markRaw(x, y, w, h); }
void Menu::invalAll(){ _markRaw(0, 0, SW, SH); }
void Menu::inval(const Rect& r){
  Page* p = top();
  if(!p) return;
  if(_tDir){ invalAll(); return; }
  /*  Запас по 2 логічні пікселі: вміст малюється зі зсувом прокрутки в пікселях
      панелі (Page::scrollP), і край може лягти на піксель-два інакше, ніж тут.  */
  int16_t y0 = r.y + HDR - p->scroll - 2, y1 = r.y + HDR - p->scroll + r.h + 2;
  if(y0 < HDR) y0 = HDR;
  if(y1 > SH) y1 = SH;
  if(y1 > y0) _markRaw(r.x - 1, y0, r.w + 2, y1 - y0);
}

void Menu::toast(const char* msg){
  strlcpy(_toast, msg ? msg : "", sizeof(_toast));
  _toastT = millis(); _toastOn = true;
  _markRaw(0, SH - 44, SW, 44);
}

int16_t Menu::_maxScroll(Page* p){
  int16_t m = p->height() - CH;
  return m > 0 ? m : 0;
}

void Menu::_startSnap(Page* p, uint32_t now){
  const int16_t st = p ? p->snapStep() : 0;
  if(st <= 0) return;
  const int16_t mx = _maxScroll(p);
  float cur = _scrollF;
  if(cur < 0) cur = 0;
  if(cur > mx) cur = mx;
  /*  куди приїхав би накат ще за мить — до найближчого рядка там; швидкість
      наката переходить у пружину, тож список перелітає й повертається  */
  const float ahead = cur + _vel * 0.12f;
  int32_t to = (int32_t)lroundf(ahead / st) * st;
  if(to < 0) to = 0;
  if(to > mx) to = mx;
  if(fabsf(to - _scrollF) < 0.5f && fabsf(_vel) < 20){ _setScroll(p, (float)to); return; }
  _spring = true; _spX = _scrollF; _spV = _vel; _spTo = (float)to; _spLast = now;
}

void Menu::_startTransition(Page* from, int8_t dir){
  _ripOn = false; _tm = TM_DEAD;
  /*  ES3C35P: нова сторінка малюється ОДИН раз у другий лист, а зсув робить сам
      вивід кадру — на кожен прохід розгортки панелі кадр зі стовпців двох листів
      (як у передатчику hearing-link). Раніше кожен кадр переходу малював обидві
      сторінки наново (~25 мс) — власник: «ефект зсуву йде слайдами».  */
  if(lcdSlideBegin()){
    _from = nullptr; _tDir = 0; _tPos = 1;
    invalAll();
    _flush();                                  /* нова сторінка — цілком у другий лист */
    lcdSlideRun(dir, T_TRANS);
    /*  дотики під час зсуву не рахуються — як і раніше (TM_DEAD до відпускання)  */
    portENTER_CRITICAL(&_mux);
    _tqT = _tqH;
    portEXIT_CRITICAL(&_mux);
    return;
  }
  _from = from; _tDir = dir; _tT0 = millis(); _tPos = 0;
  invalAll();
}

void Menu::_applyCmds(){
  for(;;){
    Cmd c;
    portENTER_CRITICAL(&_mux);
    bool any = _cmdT != _cmdH;
    if(any){ c = _cmd[_cmdT]; _cmdT = (_cmdT + 1) % QN; }
    portEXIT_CRITICAL(&_mux);
    if(!any) break;
    switch(c.op){
      case C_OPEN:
        _depth = 0; _stack[_depth++] = c.p;
        _open = true; _openReq = false;
        _tDir = 0; _from = nullptr; _tm = TM_NONE; _ripOn = false; _toastOn = false; _fling = false;
        _fade = 1; _fadeStep = 0; _fadeTick = 0;      /* погасити плеєр, намалювати, засвітити */
        break;
      case C_PUSH:
        if(_depth < MAXD){ Page* f = top(); _stack[_depth++] = c.p; if(f) _startTransition(f, 1); }
        break;
      case C_POP:
        if(_depth > 1){ Page* f = _stack[--_depth]; _startTransition(f, -1); }
        break;
      case C_REPLACE:
        if(_depth){ Page* f = _stack[_depth - 1]; _stack[_depth - 1] = c.p; _startTransition(f, 1); }
        break;
      case C_CLOSE:
        if(_open && _fade == 0){ _fade = 3; _fadeStep = 0; _fadeTick = 0; }
        break;
    }
  }
}

void Menu::_fadeRun(){
  uint32_t now = millis();
  if(_fade == 3 && (int32_t)(now - _closeAt) < 0) return;      /* хвиля ще йде */
  if(now - _fadeTick < 12) return;
  _fadeTick = now;
  const int8_t STEPS = 8;
  uint16_t full = bridgeBright();
  if(_fade == 1 || _fade == 3){
    bridgeFade(full - full * (_fadeStep + 1) / STEPS);
    if(++_fadeStep >= STEPS){
      if(_fade == 1){
        /*  у темряві — вся сторінка одним проходом  */
        invalAll();
        _flush();
        _fade = 2; _fadeStep = 0;
      }else _finishClose();
    }
  }else if(_fade == 2){
    bridgeFade(full * (_fadeStep + 1) / STEPS);
    if(++_fadeStep >= STEPS){ _fade = 0; bridgeFade(0xFFFF); }
  }
}

void Menu::_finishClose(){
  _open = false; _openReq = false;
  _depth = 0; _tDir = 0; _from = nullptr; _ripOn = false; _toastOn = false; _fling = false;
  portENTER_CRITICAL(&_mux);
  _tqT = _tqH;
  portEXIT_CRITICAL(&_mux);
  bridgeClosed();                       /* плеєр — наново, ще в темряві; головний цикл закриє сторінки */
  _fade = 2; _fadeStep = 0;
}

void Menu::_processTouch(){
  Page* p = top();
  for(;;){
    Touch t;
    portENTER_CRITICAL(&_mux);
    bool any = _tqT != _tqH;
    if(any){ t = _tq[_tqT]; _tqT = (_tqT + 1) % 24; }
    portEXIT_CRITICAL(&_mux);
    if(!any || !p) break;
    _lastTouch = t.t;
    if(t.kind == T_PRESS){
      _px0 = _plx = t.x; _py0 = _ply = t.y; _pressT = t.t; _held = false; _pressId = -1;
      _lastMoveT = t.t; _vel = 0;
      if(_tDir){ _tm = TM_DEAD; continue; }
      /*  Дотик одразу після прокрутки там само, де палець відпустили, — не натискання:
          так сенсор інколи «повторює» кінець жесту (див. також фільтр у драйвері сенсора).  */
      const bool afterScroll = (uint32_t)(t.t - _scrollEndT) < 250 && abs(t.x - _scrollEndX) < 30 && abs(t.y - _scrollEndY) < 30;
      bool caught = _fling || _spring || afterScroll;
      _fling = false; _spring = false;
      /*  «назад» притягує дотик: трохи нижче шапки й правіше самої кнопки теж  */
      if(t.y < HDR + 8 && t.x < 64 && p->canBack()) t.y = 0;
      if(t.y < HDR){
        if(t.x < 64 && p->canBack()){
          _tm = TM_BACK;
          _ripHdr = true; _ripT0 = t.t; _ripOn = true; _ripUp = false; _ripPage = p;
        }else _tm = TM_DEAD;
        continue;
      }
      int16_t cy = t.y - HDR + p->scroll;
      Rect r; uint8_t rad = 0;
      /*  палець зупинив список, що ще їхав: далі його можна вести, але цей дотик нічого не натискає  */
      int16_t id = caught ? -1 : hitNear(p, t.x, cy, r, rad);
      _pressId = id;
      _tm = TM_UNDECIDED;
      if(id >= 0){
        /*  хвиля — з найближчої до пальця точки елемента  */
        int16_t rx = t.x < r.x ? r.x : (t.x >= r.x + r.w ? r.x + r.w - 1 : t.x);
        int16_t ry = cy < r.y ? r.y : (cy >= r.y + r.h ? r.y + r.h - 1 : cy);
        _rip = r; _ripR = rad; _ripX = rx; _ripY = ry; _ripT0 = t.t; _ripOn = true; _ripUp = false; _ripHdr = false; _ripPage = p;
        if(p->grab(id) == 2){ _tm = TM_GRAB; p->drag(id, t.x, cy, false); _ripOn = false; }
      }
      /*  дробова позиція, якщо вона ще та сама (накат, пружина) — без стрибка на піксель  */
      _scrollF = lroundf(_scrollF) == p->scroll ? _scrollF : (float)p->scroll;
      _scroll0 = p->scroll;
      _dTarget = _scrollF; _dVel = 0; _dT = 0;
    }else if(t.kind == T_DRAG){
      if(_tm == TM_UNDECIDED){
        int16_t dx = t.x - _px0, dy = t.y - _py0;
        /*  Під пальцем повзунок — прокрутка забирає жест лише тоді, коли рух
            угору-вниз явно переважає (вдвічі). Інакше на ялозі панелі поперечний
            рух ловився як прокрутка: повзунок не рухався, список їхав і
            пружинив назад.  */
        const uint8_t gr = _pressId >= 0 ? p->grab(_pressId) : 0;
        const int16_t need = gr == 1 ? (int16_t)(abs(dx) * 2) : (int16_t)abs(dx);
        if(abs(dy) > 8 && abs(dy) >= need && p->scrollable() && _maxScroll(p) > 0){
          _tm = TM_SCROLL; _py0 = t.y; _ripOn = false;
          _scrollF = lroundf(_scrollF) == p->scroll ? _scrollF : (float)p->scroll;
          _scroll0 = p->scroll;
          _dTarget = _scrollF; _dVel = 0; _dT = 0; _ply = t.y; _lastMoveT = t.t;
          if(_pressId >= 0) inval(_rip);
        }else if(abs(dx) > 5 && gr == 1){
          _tm = TM_GRAB; _ripUp = true; _ripUpT = t.t;
        }else if(abs(dx) > 14 || abs(dy) > 14){
          _tm = TM_DEAD;
          if(_ripOn){ if((uint32_t)(t.t - _ripT0) < 90) _ripOn = false; else { _ripUp = true; _ripUpT = t.t; } }
        }
      }
      if(_tm == TM_SCROLL){
        float target = _scroll0 - (t.y - _py0);
        const int16_t mx = _maxScroll(p);
        if(target < 0) target *= 0.35f;
        if(target > mx) target = mx + (target - mx) * 0.35f;
        uint32_t dt = t.t - _lastMoveT;
        if(dt > 0){
          float v = -(float)(t.y - _ply) * 1000.0f / dt;
          _vel = _vel * 0.5f + v * 0.5f;                     /* для накату після відпускання */
          /*  Для самого руху швидкість згладжуємо сильніше й за часом, а не
              за кількістю дотиків: інакше рідкі й часті відліки давали б
              різну плавність.  */
          const float k = (float)dt / (float)(dt + 70);      /* стала часу 70 мс */
          _dVel += (v - _dVel) * k;
          /*  Один різкий стрибок пальця міряється як шалена швидкість —
              обмежуємо, інакше він потім довго штовхає список.  */
          if(_dVel >  1800.0f) _dVel =  1800.0f;
          if(_dVel < -1800.0f) _dVel = -1800.0f;
        }
        _ply = t.y; _lastMoveT = t.t;
        _dTarget = target;                                   /* сам список посуне такт нижче */
      }else if(_tm == TM_GRAB){
        p->drag(_pressId, t.x, t.y - HDR + p->scroll, false);
      }
      _plx = t.x;
    }else if(t.kind == T_RELEASE){
      if(_tm == TM_SCROLL){
        _scrollEndT = t.t; _scrollEndX = t.x; _scrollEndY = t.y;
        if(t.t - _lastMoveT > 90) _vel = 0;          /* палець зупинився перед відпусканням */
        _fling = true;
        if(lroundf(_scrollF) != p->scroll) _scrollF = p->scroll;   /* дробову частину лишаємо — накат без стрибка */
      }else if(_tm == TM_GRAB){
        p->drag(_pressId, _plx, t.y - HDR + p->scroll, true);
        _vel = 0; _scrollF = p->scroll;
        _startSnap(p, t.t);
      }else if(_tm == TM_UNDECIDED){
        if(_pressId >= 0 && !_held) _post(A_TAP, p, _pressId, _ripX, _ripY, 0);
      }else if(_tm == TM_BACK){
        _post(A_BACK, p, -1, 0, 0, 0);
      }
      if(_ripOn && !_ripUp){ _ripUp = true; _ripUpT = t.t; }
      _tm = TM_NONE;
    }
  }
}

void Menu::render(){
  if(!_open && !_openReq && _fade == 0) return;
  _applyCmds();
  if(_fade){
    _fadeRun();
    bool waiting = _fade == 3 && (int32_t)(millis() - _closeAt) < 0;   /* закриття чекає, поки видно хвилю */
    if(!waiting && (_fade == 1 || _fade == 3 || !_open)) return;
  }
  if(!_open) return;
  Page* p = top();
  if(!p) return;
  uint32_t now = millis();
  _processTouch();
  p = top();

  /*  перехід  */
  if(_tDir){
    float t = (now - _tT0) / (float)T_TRANS;
    if(t >= 1){ _tDir = 0; _from = nullptr; _tm = TM_NONE; }
    _tPos = t > 1 ? 1 : t;
    invalAll();
  }
  /*  довгий дотик  */
  if(_tm == TM_UNDECIDED && !_held && _pressId >= 0 && now - _pressT > 550){
    _held = true;
    _post(A_HOLD, p, _pressId, _ripX, _ripY, 0);
  }
  /*  Ведення пальцем: список іде за згладженою швидкістю, а до пальця його
      підтягує слабка пружина. Забігати вперед пальця більш ніж на кілька
      пікселів не даємо — інакше на зупинці був би помітний відкат.  */
  if(_tm == TM_SCROLL){
    float dt = _dT ? (now - _dT) / 1000.0f : 0.016f;
    _dT = now;
    if(dt > 0.05f) dt = 0.05f;
    if(now - _lastMoveT > 60) _dVel = 0;             /* палець стоїть — і список стає */
    else _dVel *= powf(0.22f, dt);
    _scrollF += _dVel * dt + (_dTarget - _scrollF) * 9.0f * dt;
    /*  Відставати дозволяємо помітно — саме в цьому запасі й гаситься ривок,
        список надолужує його приблизно за 100 мс. А обганяти палець не даємо
        зовсім: інакше після кожного ривка список проскакував на кілька
        пікселів і майже півсекунди повз назад — це було видно як гойдання.  */
    const float LAG = 0.28f * (float)smooth, LEAD = 0.5f;
    if(_scrollF - _dTarget >  LEAD) _scrollF = _dTarget + LEAD;
    if(_scrollF - _dTarget < -LAG)  _scrollF = _dTarget - LAG;
    _setScroll(p, _scrollF);
    if(_trOn && _trN < TR_N){ _trT[_trN] = (uint16_t)(now - _trT0); _trS[_trN] = p->scrollP; _trG[_trN] = (int16_t)lroundf(uiPf(_dTarget)); _trN++; }
  }else _dT = 0;
  /*  інерція прокрутки й пружина на краях  */
  if(_fling && _tm != TM_SCROLL){
    static uint32_t ft = 0;
    float dt = ft ? (now - ft) / 1000.0f : 0.016f; ft = now;
    if(dt > 0.05f) dt = 0.05f;
    const int16_t mx = _maxScroll(p);
    _scrollF += _vel * dt;
    _vel *= powf(0.06f, dt);                        /* тертя */
    if(_scrollF < 0){ _scrollF += (0 - _scrollF) * (dt * 14); _vel *= 0.7f; }
    if(_scrollF > mx){ _scrollF += (mx - _scrollF) * (dt * 14); _vel *= 0.7f; }
    _setScroll(p, _scrollF);
    if(_trOn && _trN < TR_N){ _trT[_trN] = (uint16_t)(now - _trT0); _trS[_trN] = p->scrollP; _trG[_trN] = -1; _trN++; }
    /*  сторінка з кроком віддає накат пружині ще на ходу  */
    const float stopV = p->snapStep() > 0 ? 380.0f : 15.0f;
    if(fabsf(_vel) < stopV && _scrollF >= -0.5f && _scrollF <= mx + 0.5f){
      _fling = false; ft = 0;
      if(p->scroll < 0) p->scroll = 0;
      if(p->scroll > mx) p->scroll = mx;
      _scrollF = p->snapStep() > 0 ? _scrollF : p->scroll;
      if(p->snapStep() <= 0) p->scrollP = uiP(p->scroll);      /* зупинились — на цілий логічний піксель */
      _startSnap(p, now);
      _vel = 0;
      invalScreen(0, HDR - 1, SW, CH + 1);
    }
  }
  /*  пружина до рядка (затухання 0,45, 16 рад/с): з місця — перелітає на кілька
      пікселів, з наката — помітно далі, і м'яко вертається  */
  if(_spring && _tm != TM_SCROLL){
    float dt = (now - _spLast) / 1000.0f; _spLast = now;
    if(dt > 0.06f) dt = 0.06f;
    const float w0 = 16.0f, zeta = 0.45f;
    for(float t = 0; t < dt; t += 0.004f){
      const float h = dt - t < 0.004f ? dt - t : 0.004f;
      const float a = -w0 * w0 * (_spX - _spTo) - 2 * zeta * w0 * _spV;
      _spV += a * h; _spX += _spV * h;
    }
    float f = _spX;
    if(fabsf(_spX - _spTo) < 0.5f && fabsf(_spV) < 12){ _spring = false; f = _spTo; }
    _scrollF = _spX;
    _setScroll(p, f);
    if(_trOn && _trN < TR_N){ _trT[_trN] = (uint16_t)(now - _trT0); _trS[_trN] = p->scrollP; _trG[_trN] = -2; _trN++; }
  }
  /*  хвиля  */
  if(_ripOn){
    if(_ripUp && now - _ripUpT > 260) _ripOn = false;
    if(_ripPage != p) _ripOn = false;
    else if(_ripHdr) invalScreen(0, 0, 44, HDR - 1);
    else if((int32_t)(now - _ripStart()) >= 0) inval(_rip);    /* до початку хвилі малювати нічого */
  }
  /*  повідомлення  */
  if(_toastOn){
    uint32_t age = now - _toastT;
    if(age > 2600){ _toastOn = false; _markRaw(0, SH - 44, SW, 44); }
    else if(age < 220 || age > 2380) _markRaw(0, SH - 44, SW, 44);
  }
  /*  годинник у шапці  */
  { char b[8]; uint8_t mn = 255; if(bridgeClock(b, sizeof(b), mn) && mn != _hdrMin){ _hdrMin = mn; _markRaw(SW - 110, 0, 110, HDR - 1); } }
  { int64_t t0 = esp_timer_get_time(); p->tick(now); pfTickUs += (uint32_t)(esp_timer_get_time() - t0); }
  _flush();
}

/*  Прокрутка з дробової позиції: логічна (дотики, сторінка) і в пікселях панелі
    (малювання). Перемальовуємо, щойно зрушив хоч один піксель панелі — на
    ES3C35P це вдвічі-втричі частіші, зате рівні кроки.  */
bool Menu::_setScroll(Page* p, float f){
  const int16_t s = (int16_t)lroundf(f);
  const int16_t sp = (int16_t)lroundf(uiPf(f));
  if(s == p->scroll && sp == p->scrollP) return false;
  p->scroll = s; p->scrollP = sp;
  invalScreen(0, HDR - 1, SW, CH + 1);
  return true;
}

void Menu::_flush(){
  uint32_t rows[TROWS];
  portENTER_CRITICAL(&_mux);
  memcpy(rows, _dirty, sizeof(rows));
  memset(_dirty, 0, sizeof(_dirty));
  portEXIT_CRITICAL(&_mux);
  bool any = false;
  for(int r = 0; r < TROWS; r++) if(rows[r]){ any = true; break; }
  if(!any) return;
  /*  Спільний буфер ділимо навпіл: смуга по 16 рядків малюється в одну
      половину, поки попередня йде шиною з другої. Шина (40 МГц) — найдовша
      частина кадру, і малювання раніше чекало на неї без діла.  */
  uint16_t* scr = (uint16_t*)lcdScratch((size_t)STRIP_PX * 2);
  if(!scr) return;
  /*  ES3C28P: дві половини по 16 рядків (одна йде шиною, у другу малюємо);
      ES3C35P: одна смуга 32 рядки — поворот однаково копіює її у свої буфери.
      Менше смуг — менше разів обходити всю сцену: на 480×320 це головне.  */
  const int32_t HALF = LCD_PUSH_COPIES ? STRIP_PX : STRIP_PX / 2;
  uint16_t* half[2] = { scr, LCD_PUSH_COPIES ? scr : scr + HALF };
  uint8_t hi = 0;
  Gfx g;
  const int64_t f0 = esp_timer_get_time();
  _frameT = millis();                        /* одна мить на весь кадр — хвиля й повідомлення не рвуться між смугами */
  g_m2Draw = true;
  lcdBegin();
  for(int r = 0; r < TROWS; r++){
    while(rows[r]){
      uint32_t m = rows[r];
      int s = __builtin_ctz(m), e = s;
      while(e < TCOLS && ((m >> e) & 1)) e++;
      uint32_t run = (((1UL << (e - s)) - 1) << s);
      rows[r] &= ~run;
      int16_t x0 = s * 16, w = (e - s) * 16;
      if(x0 + w > PSW) w = PSW - x0;
      int16_t y0 = r * 16, h = 16;
      for(int rr = r + 1; rr < TROWS && (rows[rr] & run) == run && (int32_t)w * (h + 16) <= HALF; rr++){ rows[rr] &= ~run; h += 16; }
      if(y0 + h > PSH) h = PSH - y0;
      /*  Широкий рядок плиток (480 на ES3C35P) у пів-буфера цілим не входить —
          виводимо його шматками по рядках (кратними LCD_ROW_ALIGN).  */
      const int16_t step = lcdRowsFor(w, HALF);
      for(int16_t yy = 0; yy < h && step > 0; yy += step){
        const int16_t hh = (h - yy < step) ? (h - yy) : step;
        /*  Можна малювати й прямо в кадр у PSRAM (lcdFrameFor), але заміряно 26.10.2026:
            малювання в PSRAM 13 мс замість 6,7, а копіювання смуги — 6,6 мс; разом те
            саме (вузьке місце — сама PSRAM). Лишаємо перевірений шлях через внутрішню смугу.  */
        uint16_t* fbp = nullptr;
        uint16_t* buf = fbp ? fbp : half[hi];
        g.target(buf, x0, y0 + yy, w, hh, fbp ? lcdFrameStride() : 0);
        int64_t u0 = esp_timer_get_time();
        _drawScene(g);
        int64_t u1 = esp_timer_get_time();
        if(fbp) lcdMarked(x0, y0 + yy, w, hh);
        else if(lcdPush(x0, y0 + yy, w, hh, buf)) hi ^= 1;
        pfDrawUs += (uint32_t)(u1 - u0); pfXferUs += (uint32_t)(esp_timer_get_time() - u1); pfStrips++;
      }
    }
  }
  { int64_t w0 = esp_timer_get_time(); lcdEnd(); pfXferUs += (uint32_t)(esp_timer_get_time() - w0); }
  g_m2Draw = false;
  { uint32_t us = (uint32_t)(esp_timer_get_time() - f0); if(us > pfMaxUs) pfMaxUs = us; }
  pfFrames++; g_m2Frames++;
}

void Menu::_drawHeader(Gfx& g, Page* p){
  if(!g.visible(0, 0, SW, HDR)) return;
  if(p->canBack()){
    g.circle(20, 20, 14, C_SURF);
    icon(g, _depth <= 1 && p == _stack[0] ? IC_DOWN : IC_BACK, 19, 20, C_TXT, C_SURF);
  }
  int16_t tx = p->canBack() ? 42 : 14;
  char clk[8]; uint8_t mn = 0;
  int16_t rightW = 0;
  if(bridgeClock(clk, sizeof(clk), mn)){
    rightW = g.text(SW - 12, 25, clk, F_SMB, C_TXT2, AL_R);
    int8_t rs = bridgeRssi();
    uint8_t lv = rs == 0 ? 0 : rs > -55 ? 4 : rs > -65 ? 3 : rs > -75 ? 2 : 1;
    signalBars(g, SW - 12 - rightW - 22, 24, lv, C_TXT2, C_SURF2);
    rightW += 30;
  }
  g.text(tx, 26, p->title(), F_TITLE, C_TXT, AL_L, SW - tx - rightW - 18);
  if(p->scroll > 0) g.fill(0, HDR - 1, SW, 1, C_LINE);
}

void Menu::_drawPage(Gfx& g, Page* p, float dxf, bool overlays){
  if(!p) return;
  /*  зсув сторінки (перехід) — логічний для сторінки й окремо в пікселях панелі  */
  const int16_t dx = (int16_t)lroundf(dxf), dxP = (int16_t)lroundf(uiPf(dxf));
  g.origin(0, 0);
  Rect s0 = g.narrowP(dxP, 0, dxP + PSW, PSH);
  if(g.clipRect().empty()){ g.restore(s0); return; }
  g.originP(dx, 0, dxP, 0);
  g.vgrad(0, 0, SW, 90, C_BGTOP, C_BG);
  g.fill(0, 90, SW, SH - 90, C_BG);
  _drawHeader(g, p);
  if(overlays && _ripOn && _ripHdr && _ripPage == p){
    int32_t up = _frameT - _ripUpT;
    int a = !_ripUp ? 80 : (up >= 260 ? 0 : (int)(80 * (1 - up / 260.0f)));
    if(a > 0) g.lighten(Rect(6, 6, 28, 28), 14, 20, 20, 20, (uint8_t)a);
  }
  Rect s1 = g.narrow(0, HDR, SW, CH);
  /*  прокрутка — у пікселях панелі (Page::scrollP); якщо її змінили в обхід Menu — з логічної  */
  int16_t sP = p->scrollP;
  if(sP - uiP(p->scroll) > 1 || uiP(p->scroll) - sP > 1) sP = uiP(p->scroll);
  g.originP(dx, HDR - p->scroll, dxP, uiP(HDR) - sP);
  p->draw(g);
  if(overlays && _ripOn && !_ripHdr && _ripPage == p && (int32_t)(_frameT - _ripStart()) >= 0){
    uint32_t now = _frameT;
    float k = (now - _ripStart()) / 280.0f; if(k > 1) k = 1;
    float e = easeOut(k);
    float dxm = _ripX - _rip.x > _rip.x + _rip.w - _ripX ? _ripX - _rip.x : _rip.x + _rip.w - _ripX;
    float dym = _ripY - _rip.y > _rip.y + _rip.h - _ripY ? _ripY - _rip.y : _rip.y + _rip.h - _ripY;
    float rmax = sqrtf(dxm * dxm + dym * dym) + 2;
    float rad = 8 + (rmax - 8) * e;
    int a = 58;
    if(_ripUp){ int32_t up = now - _ripUpT; a = up >= 260 ? 0 : (int)(58 * (1 - up / 260.0f)); }
    if(a > 0) g.lighten(_rip, _ripR, _ripX, _ripY, rad, (uint8_t)a);
  }
  /*  смужка прокрутки  */
  const int16_t mx = _maxScroll(p);
  if(overlays && mx > 0 && (_tm == TM_SCROLL || _fling)){
    g.originP(dx, HDR, dxP, uiP(HDR));
    int16_t ph = p->height();
    int16_t bh = (int16_t)((int32_t)CH * CH / ph); if(bh < 18) bh = 18;
    int16_t s = p->scroll < 0 ? 0 : (p->scroll > mx ? mx : p->scroll);
    int16_t by = (int16_t)((int32_t)(CH - bh - 8) * s / mx) + 4;
    g.box(SW - 5, by, 3, bh, 1, C_TXT3);
  }
  g.restore(s1);
  g.origin(0, 0);
  g.restore(s0);
}

void Menu::_drawScene(Gfx& g){
  if(s_cal.active){ _drawCalib(g); return; }
  Page* p = top();
  if(_tDir && _from){
    float e = easeOut(_tPos);
    /*  Межа між сторінками й зсуви — у пікселях панелі з дробової позиції:
        сторінка їде рівно, по пікселю панелі, а не стрибками по 1–2.  */
    if(_tDir > 0){
      const float dxTo = (1 - e) * SW, dxFrom = -e * SW * 0.3f;
      const int16_t cut = (int16_t)lroundf(uiPf(dxTo));
      Rect s = g.narrowP(0, 0, cut, PSH);
      _drawPage(g, _from, dxFrom, false);
      g.origin(0, 0); g.fillA(0, 0, SW, SH, 0x0000, (uint8_t)(e * 120));
      g.restore(s);
      s = g.narrowP(cut, 0, PSW, PSH);
      _drawPage(g, p, dxTo, false);
      g.restore(s);
    }else{
      const float dxFrom = e * SW, dxTo = -(1 - e) * SW * 0.3f;
      const int16_t cut = (int16_t)lroundf(uiPf(dxFrom));
      Rect s = g.narrowP(0, 0, cut, PSH);
      _drawPage(g, p, dxTo, false);
      g.origin(0, 0); g.fillA(0, 0, SW, SH, 0x0000, (uint8_t)((1 - e) * 120));
      g.restore(s);
      s = g.narrowP(cut, 0, PSW, PSH);
      _drawPage(g, _from, dxFrom, false);
      g.restore(s);
    }
  }else _drawPage(g, p, 0, true);
  /*  повідомлення знизу  */
  if(_toastOn){
    uint32_t age = _frameT - _toastT;
    float k = age < 220 ? easeOut(age / 220.0f) : (age > 2380 ? 1 - (age - 2380) / 220.0f : 1);
    if(k < 0) k = 0;
    int16_t tw = Gfx::textW(_toast, F_ROW) + 32; if(tw > SW - 24) tw = SW - 24;
    int16_t ty = SH - (int16_t)(40 * k);
    g.origin(0, 0);
    g.box((SW - tw) / 2, ty, tw, 32, 16, C_SURF2);
    g.text(SW / 2, ty + 20, _toast, F_ROW, C_TXT, AL_C, tw - 20);
  }
}


/*  ---------- калібрування: реалізація ---------- */

bool Menu::calibActive() const { return s_cal.active; }

void Menu::calibStart(){
  s_cal.active = true; s_cal.step = 0; s_cal.err = 0;
  s_cal.lastT = millis(); _lastTouch = millis();
  invalAll();
}

void Menu::calibReset(){
  ExtStore& s = extras.s;
  s.tsCalXL = TS_CAL_LO; s.tsCalXR = TS_CAL_XHI;
  s.tsCalYT = TS_CAL_LO; s.tsCalYB = TS_CAL_YHI;
  s.tsCalInit = 1; extras.changed();
  toast("Калібрування скинуто");
}

void Menu::calibDown(int16_t x, int16_t y){
  if(!s_cal.active || s_cal.step >= 4) return;
  s_cal.lastT = millis(); _lastTouch = millis();
  s_cal.err = 0;
  s_cal.mx[s_cal.step] = x; s_cal.my[s_cal.step] = y;
  s_cal.step++;
  if(s_cal.step < 4){ invalAll(); return; }

  /*  Зібрали чотири точки — рахуємо середні по краях і перевіряємо.  */
  const int16_t XL = (s_cal.mx[0] + s_cal.mx[3]) / 2;   /* ліві позначки */
  const int16_t XR = (s_cal.mx[1] + s_cal.mx[2]) / 2;   /* праві */
  const int16_t YT = (s_cal.my[0] + s_cal.my[1]) / 2;   /* верхні */
  const int16_t YB = (s_cal.my[2] + s_cal.my[3]) / 2;   /* нижні */
  const int spanX = XR - XL, spanY = YB - YT;
  /*  Пари з одного краю мають збігатися; розкид — некалібрувати.  */
  const bool pairOk = abs(s_cal.mx[0] - s_cal.mx[3]) < 70 && abs(s_cal.mx[1] - s_cal.mx[2]) < 70 &&
                      abs(s_cal.my[0] - s_cal.my[1]) < 70 && abs(s_cal.my[2] - s_cal.my[3]) < 70;
  const bool spanOk = spanX >= 80 && spanX <= 500 && spanY >= 60 && spanY <= 400;
  if(pairOk && spanOk){
    ExtStore& s = extras.s;
    s.tsCalXL = XL; s.tsCalXR = XR; s.tsCalYT = YT; s.tsCalYB = YB;
    s.tsCalInit = 1; extras.changed();
    s_cal.active = false;
    toast("Калібрування збережено");
    Serial.printf("##TOUCH#\tкалібр: X %d..%d  Y %d..%d\n", XL, XR, YT, YB);
  }else{
    /*  Невдало — старе калібрування лишаємо, просимо повторити.  */
    s_cal.step = 0; s_cal.err = 1;
    toast("Неточно — почніть спочатку");
    Serial.printf("##TOUCH#\tкалібр невдало: spanX=%d spanY=%d pairOk=%d\n", spanX, spanY, (int)pairOk);
  }
  invalAll();
}

static void calibTarget(Gfx& g, int16_t cx, int16_t cy, uint16_t c){
  g.line(cx - 12, cy, cx + 12, cy, 2.0f, c);
  g.line(cx, cy - 12, cx, cy + 12, 2.0f, c);
  g.circle(cx, cy, 9, c);
  g.circle(cx, cy, 6, C_BG);
  g.circle(cx, cy, 2.5f, c);
}

void Menu::_drawCalib(Gfx& g){
  g.origin(0, 0);
  g.fill(0, 0, SW, SH, C_BG);
  /*  завершені позначки — тьмяні зелені  */
  /*  позначки стоять у пікселях панелі (там же міряються сирі дотики) — малюємо в логічних  */
  for(uint8_t i = 0; i < s_cal.step && i < 4; i++) calibTarget(g, uiL(CAL_TX[i]), uiL(CAL_TY[i]), C_GREEN);
  /*  поточна — яскрава жовта  */
  if(s_cal.step < 4) calibTarget(g, uiL(CAL_TX[s_cal.step]), uiL(CAL_TY[s_cal.step]), C_ACC);
  /*  підказка по центру  */
  g.text(SW / 2, SH / 2 - 14, "Калібрування сенсора", F_TITLE, C_TXT, AL_C);
  char b[64];
  snprintf(b, sizeof(b), tr("Торкніться позначки  %u/4"), (unsigned)(s_cal.step < 4 ? s_cal.step + 1 : 4));
  g.text(SW / 2, SH / 2 + 12, b, F_ROW, C_TXT2, AL_C);
  if(s_cal.err) g.text(SW / 2, SH / 2 + 34, "Торкайтесь точно центру позначки", F_SMB, C_ACC, AL_C);
}


/*  ---------- службове: перелік елементів і пряма прокрутка (консоль) ---------- */

static const char* itName(uint8_t t){
  switch(t){
    case IT_SECTION: return "section";
    case IT_NAV:     return "nav";
    case IT_SWITCH:  return "switch";
    case IT_SLIDER:  return "slider";
    case IT_SEG:     return "seg";
    case IT_BUTTON:  return "button";
    case IT_INFO:    return "info";
    case IT_NOTE:    return "note";
    case IT_CUSTOM:  return "custom";
    default:         return "gap";
  }
}

void ListPage::dump(){
  for(uint8_t i = 0; i < _n; i++){
    Item& it = _it[i];
    if(!it.vis) continue;
    long v = it.get ? (long)it.get() : 0;
    Serial.printf("ITEM\t%u\t%s\t%d\t%d\t%ld\t%ld\t%ld\t%d\t%s\n",
                  (unsigned)i, itName(it.type), (int)it.y, (int)it.hh,
                  it.get ? v : (long)0, (long)it.lo, (long)it.hi,
                  (it.enabled && !it.enabled()) ? 0 : 1,
                  it.label ? it.label : "");
  }
}

void Menu::dumpTop(){
  Page* p = top();
  if(!p){ Serial.println("PAGE\t-\t0\t0"); return; }
  Serial.printf("PAGE\t%s\t%d\t%d\n", p->title(), (int)p->scroll, (int)p->height());
  p->dump();
  Serial.println("ENDPAGE");
}

void Menu::traceStart(){ _trN = 0; _trT0 = millis(); _trOn = true; }
void Menu::traceDump(){
  _trOn = false;
  Serial.printf("SCRTRACE тактів %u  (мс: список -> палець, пікселі панелі; -1 накат, -2 пружина)\n", (unsigned)_trN);
  for(uint8_t i = 0; i < _trN; i++) Serial.printf("  %4u: %4d -> %4d\n", (unsigned)_trT[i], (int)_trS[i], (int)_trG[i]);
}

void Menu::setScroll(int16_t s){
  Page* p = top();
  if(!p) return;
  const int16_t mx = _maxScroll(p);
  if(s < 0) s = 0;
  if(s > mx) s = mx;
  p->scroll = s; p->scrollP = uiP(s); _scrollF = s; _fling = false; _spring = false;
  _dTarget = s; _dVel = 0; _dT = 0;
  invalAll();
}

}  // namespace m2
