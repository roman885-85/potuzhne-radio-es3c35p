/*  Нове меню: «Меню» (пульт), «Параметри», «Екран», «Будильник», «Обране»,
    «Проповіді», «Про радіо», «Живлення», «Часовий пояс».  */
#include "../core/options.h"
#include "m2pages.h"
#include "m2bridge.h"
#include "m2lang.h"
#include <SPIFFS.h>
#include "../core/config.h"
#include "../core/player.h"
#include "../core/network.h"
#include "../core/timekeeper.h"
#include "../extras/yoExtras.h"
#include "../extras/yoDlna.h"
#include "../extras/yoAirplay.h"
#include "../extras/yoRecorder.h"
#include "../extras/yoSermons.h"
#include "../extras/yoLogos.h"
#include "../extras/yoVersion.h"
#include "../extras/yoOta.h"
#include "../core/display.h"
#include "esp_heap_caps.h"

namespace m2 {

volatile uint8_t afterClose = 0;

static const uint16_t SLEEP_MIN[5] = { 0, 15, 30, 60, 90 };

/*  =================== барабан часу =================== */
static const int16_t DP = 34;          /* крок рядка барабана */

static int16_t wrapv(int16_t v, int16_t n){ return (int16_t)(((v % n) + n) % n); }

int16_t Drum::at(uint8_t c) const { return wrapv((int16_t)lroundf(pos[c]), n[c]); }

void Drum::draw(Gfx& g){
  if(!g.visible(MX, top, CWID, h)) return;
  g.box(MX, top, CWID, h, R_CARD, C_SURF);
  const int16_t cy = top + h / 2;
  g.box(MX + 14, cy - 21, CWID - 28, 42, 12, C_SURF2);
  Rect s = g.narrow(MX, top + 2, CWID, h - 4);
  for(uint8_t c = 0; c < 2; c++){
    const int16_t cx = SW / 2 + (c ? 56 : -56);
    const int16_t base = (int16_t)floorf(pos[c]);
    for(int16_t k = -3; k <= 3; k++){
      int16_t v = base + k;
      float dy = (v - pos[c]) * DP;
      if(fabsf(dy) > h / 2 + 12) continue;
      const char* t = label ? label(c, wrapv(v, n[c])) : "";
      float a = 1 - fabsf(dy) / (DP * 2.4f); if(a < 0) a = 0;
      if(fabsf(dy) < DP * 0.5f) g.text(cx, (int16_t)(cy + dy + 11), t, F_BIG, C_TXT, AL_C);
      else g.text(cx, (int16_t)(cy + dy + 8), t, F_MID, Gfx::blend(C_SURF, C_TXT2, (uint8_t)(a * 255)), AL_C);
    }
  }
  g.restore(s);
  g.text(SW / 2, cy + 9, ":", F_BIG, C_ACC, AL_C);
}

void Drum::press(int16_t x, int16_t y){
  col = x < SW / 2 ? 0 : 1;
  drag = true; y0 = lastY = y; p0 = pos[col]; vel = 0; lastT = millis();
  snapping[col] = false;
}

void Drum::move(int16_t y){
  if(!drag) return;
  uint32_t now = millis();
  pos[col] = p0 - (y - y0) / (float)DP;
  uint32_t dt = now - lastT;
  if(dt > 0){ float v = -(float)(y - lastY) / DP * 1000.0f / dt; vel = vel * 0.5f + v * 0.5f; }
  lastY = y; lastT = now;
}

void Drum::release(){
  if(!drag) return;
  drag = false;
  if(abs(lastY - y0) < 5){
    /*  дотик без руху: вище середини — попереднє значення, нижче — наступне  */
    const int16_t cy = top + h / 2;
    if(y0 < cy - 22) pos[col] = lroundf(pos[col]) - 1;
    else if(y0 > cy + 22) pos[col] = lroundf(pos[col]) + 1;
    vel = 0;
  }
  if(millis() - lastT > 90) vel = 0;
  snapping[col] = true;
  val[col] = (int16_t)lroundf(pos[col] + vel * 0.12f);        /* ціль (без обмеження — колонка по колу) */
}

bool Drum::tick(uint32_t now){
  (void)now;
  bool moving = drag;
  for(uint8_t c = 0; c < 2; c++){
    if(drag && c == col) continue;
    float target = snapping[c] ? (float)val[c] : (float)val[c];
    float d = target - pos[c];
    if(fabsf(d) < 0.002f){
      if(snapping[c]){ snapping[c] = false; val[c] = wrapv(val[c], n[c]); pos[c] = val[c]; }
      continue;
    }
    pos[c] += d * 0.3f;
    if(fabsf(target - pos[c]) < 0.01f) pos[c] = target;
    moving = true;
  }
  return moving;
}

/*  =================== «Меню» — пульт =================== */
class PultPage : public Page {
  public:
    const char* title() override { return "Меню"; }
    void enter() override { _sig = 0; _briA = -1; }
    void draw(Gfx& g) override;
    void tick(uint32_t now) override;
    bool scrollable() override { return false; }
    int16_t hit(int16_t x, int16_t y, Rect& r, uint8_t& radius) override;
    uint8_t grab(int16_t id) override { return id == 10 ? 1 : 0; }
    void drag(int16_t id, int16_t x, int16_t y, bool end) override;
    void tap(int16_t id, int16_t x, int16_t y) override;
    bool hold(int16_t id) override;
    void value(int16_t id, int32_t v) override;
    bool canBack() override { return true; }
  private:
    uint32_t _sig = 0, _sigT = 0;
    float _briA = -1; int16_t _briDrag = -1; uint32_t _briHold = 0;
    static Rect tile(uint8_t i){ int16_t w = (CWID - 24) / 4; return Rect(MX + i * (w + 8), 4, w, 64); }
    /*  розділи — 4 × 2: оновлення й живлення тут же, а не лише в «Параметрах»  */
    static Rect card(uint8_t i){ int16_t w = (CWID - 24) / 4; return Rect(MX + (i % 4) * (w + 8), 114 + (i / 4) * 44, w, 40); }
    static int briAt(int16_t x){ const float x0 = MX + 40 + 9 + 6, w = CWID - 40 - 50 - 18 - 12; float f = (x - x0) / w; if(f < 0) f = 0; if(f > 1) f = 1; return 5 + lroundf(f * 95); }   /* краї липкі */
};

static bool sdOff(){ return extras.s.noSd; }

void PultPage::draw(Gfx& g){
  char b[24];
  /*  швидкі перемикачі  */
  for(uint8_t i = 0; i < 4; i++){
    Rect r = tile(i);
    if(!g.visible(r.x, r.y, r.w, r.h)) continue;
    bool on = false, dis = false; uint8_t ic = IC_MOON; const char* lb = ""; b[0] = 0;
    switch(i){
      case 0: {
        uint16_t m = extras.sleepMinutes();
        on = m > 0; ic = IC_MOON; lb = "Сон";
        if(!m) snprintf(b, sizeof(b), "вимк");
        else { uint32_t s = extras.sleepLeftSec(); snprintf(b, sizeof(b), "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60)); }
        break; }
      case 1:
        on = extras.s.alarmOn; ic = IC_ALARM; lb = "Будильник";
        if(on) snprintf(b, sizeof(b), "%02u:%02u", extras.s.alarmH, extras.s.alarmM); else snprintf(b, sizeof(b), "вимк");
        break;
      case 2:
        on = recorder.active(); dis = sdOff(); ic = IC_REC; lb = "Запис";
        if(on){ uint32_t s = recorder.seconds(); snprintf(b, sizeof(b), "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60)); }
        else snprintf(b, sizeof(b), dis ? "без картки" : "вимк");
        break;
      default:
        on = config.getMode() == PM_SDCARD; dis = sdOff(); ic = on ? IC_CARD : IC_RADIO; lb = on ? "Картка" : "Радіо";
        snprintf(b, sizeof(b), dis ? "лише радіо" : (on ? "з картки" : "з мережі"));
        break;
    }
    uint16_t bg = on ? (i == 2 ? C_REC : C_ACC) : C_SURF;
    uint16_t fg = on ? (i == 2 ? 0xFFFF : C_ACCTXT) : (dis ? C_TXT3 : C_TXT);
    uint16_t sub = on ? (i == 2 ? 0xFFFF : RGB(80, 72, 30)) : C_TXT2;
    uint16_t icc = on ? fg : (dis ? C_TXT3 : C_ACC);
    g.box(r.x, r.y, r.w, r.h, 14, bg);
    icon(g, ic, r.x + 17, r.y + 17, icc, bg);
    g.text(r.x + 9, r.y + 45, lb, F_SMB, fg, AL_L, r.w - 12);
    g.text(r.x + 9, r.y + 58, b, F_SM, sub, AL_L, r.w - 12);
  }
  /*  яскравість  */
  if(g.visible(MX, 76, CWID, 34)){
    g.box(MX, 76, CWID, 34, R_CARD, C_SURF);
    icon(g, IC_SUN, MX + 20, 93, C_TXT2, C_SURF);
    float v = _briA < 0 ? config.store.brightness : _briA;
    drawSlider(g, MX + 40, 93, CWID - 40 - 50, (v - 5) / 95.0f);
    snprintf(b, sizeof(b), "%d%%", (int)lroundf(v));
    g.text(MX + CWID - 12, 97, b, F_SMB, C_TXT, AL_R);
  }
  /*  розділи  */
  static const char* const NAME[8] = { "Станції", "Обране", "Проповіді", "Звук", "Екран", "Параметри", "Оновлення", "Живлення" };
  static const uint8_t IC[8] = { IC_LIST, IC_STAR, IC_CROSS, IC_EQ, IC_SUN, IC_GEAR, IC_REFRESH, IC_POWER };
  static const uint16_t COL[8] = { C_ORANGE, C_ACC, C_VIOLET, C_BLUE, C_TEAL, C_GREY, C_BLUE, C_RED };
  for(uint8_t i = 0; i < 8; i++){
    Rect r = card(i);
    if(!g.visible(r.x, r.y, r.w, r.h)) continue;
    const bool upd = i == 6 && (ota.available() || ota.installing());
    uint16_t col = upd ? C_ACC : COL[i];
    g.box(r.x, r.y, r.w, r.h, R_CARD, upd ? Gfx::blend(C_SURF, C_ACC, 40) : C_SURF);
    const int16_t cx = r.x + r.w / 2;
    g.box(cx - 11, r.y + 4, 22, 22, R_BADGE, col);
    icon(g, IC[i], cx, r.y + 15, col == C_ACC ? C_ACCTXT : 0xFFFF, col);
    if(upd) g.circle(cx + 12, r.y + 5, 4.5f, C_RED);            /* є нова версія */
    g.text(cx, r.y + 36, upd && ota.installing() ? "іде…" : NAME[i], F_SMB, upd ? C_ACC : C_TXT, AL_C, r.w - 4);
  }
}

void PultPage::tick(uint32_t now){
  if(now - _sigT >= 250){
    _sigT = now;
    uint32_t s = extras.sleepMinutes() ? extras.sleepLeftSec() + 1 : 0;
    s = s * 31 + (extras.s.alarmOn ? extras.s.alarmH * 60 + extras.s.alarmM + 1 : 0);
    s = s * 31 + (recorder.active() ? recorder.seconds() + 1 : 0);
    s = s * 31 + config.getMode() * 2 + (extras.s.noSd ? 1 : 0);
    if(s != _sig){ _sig = s; M.inval(Rect(0, 0, SW, 70)); }
    static uint8_t updWas = 255;
    const uint8_t upd = (ota.available() ? 1 : 0) + (ota.installing() ? 2 : 0);
    if(upd != updWas){ updWas = upd; M.inval(card(6)); }
  }
  float target = (_briDrag >= 0 && now < _briHold) ? _briDrag : config.store.brightness;
  if(_briA < 0) _briA = target;
  else if(fabsf(_briA - target) > 0.01f){
    _briA += (target - _briA) * 0.35f;
    if(fabsf(_briA - target) < 0.5f) _briA = target;
    M.inval(Rect(0, 76, SW, 34));
  }
}

int16_t PultPage::hit(int16_t x, int16_t y, Rect& r, uint8_t& radius){
  for(uint8_t i = 0; i < 4; i++){ Rect t = tile(i); if(t.has(x, y)){ r = t; radius = 14; return i; } }
  if(y >= 76 && y < 110){ r = Rect(MX, 76, CWID, 34); radius = R_CARD; return 10; }
  for(uint8_t i = 0; i < 8; i++){ Rect t = card(i); if(t.has(x, y)){ r = t; radius = R_CARD; return 20 + i; } }
  return -1;
}

void PultPage::drag(int16_t id, int16_t x, int16_t y, bool end){
  (void)y;
  if(id != 10) return;
  _briDrag = briAt(x); _briHold = millis() + (end ? 700 : 100000);
  M.inval(Rect(0, 76, SW, 34));
  M.postValue(10, _briDrag, end);
}

void PultPage::value(int16_t id, int32_t v){
  if(id != 10) return;
  config.store.brightness = (uint8_t)v;
  config.setBrightness(true);
}

void PultPage::tap(int16_t id, int16_t x, int16_t y){
  (void)y;
  switch(id){
    case 0: {                                   /* сон: по колу 15 → 30 → 60 → 90 → вимк */
      uint16_t m = extras.sleepMinutes(), nx = 15;
      for(uint8_t i = 0; i < 5; i++) if(SLEEP_MIN[i] == m){ nx = SLEEP_MIN[(i + 1) % 5]; break; }
      extras.setSleep(nx);
      char b[40];
      if(nx) snprintf(b, sizeof(b), tr("таймер сну: %u хв"), nx); else snprintf(b, sizeof(b), "таймер сну вимкнено");
      M.toast(b);
      break; }
    case 1: M.push(&pgAlarm); break;
    case 2:
      if(extras.s.noSd){ M.toast("картку вимкнено в «Розробнику»"); break; }
      if(recorder.active()){ recorder.stop(); M.toast("запис зупинено"); }
      else if(!recorder.start()) M.toast(recorder.lastError());
      else M.toast("пишу ефір на картку");
      break;
    case 3:
      if(extras.s.noSd){ M.toast("картку вимкнено в «Розробнику»"); break; }
      config.changeMode();
      M.close();
      break;
    case 10: value(10, briAt(x)); break;
    case 20: M.push(&pgStations); break;
    case 21: M.push(&pgFav); break;
    case 22:
      if(!sermons.loading() && sermons.count() == 0) sermons.fetch();
      M.push(&pgSermons); break;
    case 23: M.push(&pgEq); break;
    case 24: M.push(&pgScreen); break;
    case 25: M.push(&pgSettings); break;
    case 26: M.push(&pgUpdate); break;
    case 27: M.push(&pgPower); break;
  }
}

bool PultPage::hold(int16_t id){
  if(id == 0){ M.push(&pgAlarm); return true; }
  return false;
}

static PultPage s_pult;
Page& pgPult = s_pult;

/*  =================== «Параметри» =================== */
static const char* vWifi(){ return WB::staUp() ? WB::curSsid() : "немає"; }
static const char* vMic(){ return extras.s.micOn ? "увімк" : "вимк"; }
static const char* vGest(){
  const ExtStore& e = extras.s;
  if(e.clapOn && e.knockOn) return "хлопки, стук";
  if(e.clapOn) return "хлопки";
  if(e.knockOn) return "стук";
  return "вимк";
}
static const char* vPres(){ return (extras.s.sleepEar || extras.s.presWake || extras.s.presOff) ? "увімк" : "вимк"; }
static const char* vTz(){ static char b[12]; snprintf(b, sizeof(b), "%+03d:%02d", config.store.tzHour, abs(config.store.tzMin)); return b; }
static const char* vVer(){ return prVersion(); }
static const char* vUpd(){
  static char b[40];
  if(ota.installing()) return "іде…";
  if(ota.available()){ snprintf(b, sizeof(b), tr("є %s"), ota.latest()); return b; }
  if(ota.state() == OTA_CHECKING) return "перевіряю…";
  return ota.latest()[0] ? "остання" : "";
}

/*  Назви мов навмисно не перекладаються: кожна написана сама собою, щоб її
    впізнав і той, хто другої не знає.  */
static const char* const LANGS[] = { "Українська", "English" };

static Item s_setItems[] = {
  iSection("МЕРЕЖА"),
  iNav("Wi-Fi", IC_WIFI, C_BLUE, vWifi, [](){ M.push(&pgWifi); }),
  iSection("ГОЛОС І ЖЕСТИ"),
  iNav("Мікрофон", IC_MIC, C_VIOLET, vMic, [](){ M.push(&pgMic); }),
  iNav("Хлопки й стук", IC_HAND, C_VIOLET, vGest, [](){ M.push(&pgGest); }),
  iNav("Присутність", IC_PERSON, C_VIOLET, vPres, [](){ M.push(&pgPres); }),
  iSection("СИСТЕМА"),
  iSeg("Мова", LANGS, 2, [](){ return (int32_t)extras.s.lang; },
       [](int32_t v){ extras.s.lang = (uint8_t)v; extras.changed(); langSet((uint8_t)v); M.invalAll(); }),
  iNav("Часовий пояс", IC_GLOBE, C_TEAL, vTz, [](){ M.push(&pgTz); }),
  iSwitch("Автостарт", IC_START, C_TEAL, [](){ return (int32_t)(config.store.smartstart != 2); },
          [](int32_t v){ config.saveValue(&config.store.smartstart, static_cast<uint8_t>(v ? 1 : 2)); }),
  iSwitch("Інфо про потік", IC_INFO, C_TEAL, [](){ return (int32_t)config.store.audioinfo; },
          [](int32_t v){ config.saveValue(&config.store.audioinfo, static_cast<bool>(v)); }),
  iSwitch("Колонка DLNA", IC_SPEAKER, C_BLUE, [](){ return (int32_t)(dlna.on() ? 1 : 0); },
          [](int32_t v){ dlna.setOn(v != 0); M.toast(v ? "радіо видно в мережі як колонку" : "колонку вимкнено"); }),
  iNote([](){ return "телефон чи комп'ютер надсилає радіо доріжку\n(BubbleUPnP, VLC, «Передати на пристрій»)"; }, 36),
  iSwitch("Колонка AirPlay", IC_SPEAKER, C_BLUE, [](){ return (int32_t)(airplay.on() ? 1 : 0); },
          [](int32_t v){ airplay.setOn(v != 0); M.toast(v ? "радіо видно в AirPlay" : "AirPlay вимкнено"); }),
  iNote([](){ return "iPhone, iPad і Mac грають на радіо будь-який звук\n(«Звук» у Пункті керування, кнопка AirPlay)"; }, 36),
  iSection("РАДІО"),
  iNav("Оновлення", IC_REFRESH, C_BLUE, vUpd, [](){ M.push(&pgUpdate); }),
  iNav("Про радіо", IC_INFO, C_GREY, vVer, [](){ M.push(&pgInfo); }),
  iNav("Живлення", IC_POWER, C_RED, nullptr, [](){ M.push(&pgPower); }),
  iNav("Розробник", IC_CODE, C_ACC, nullptr, [](){ M.push(&pgDev); }),
};
static ListPage s_settings("Параметри", s_setItems, sizeof(s_setItems) / sizeof(s_setItems[0]));
Page& pgSettings = s_settings;

/*  =================== «Екран» =================== */
static const char* const SAVE_LBL[5] = { "вимк", "10 с", "15 с", "30 с", "60 с" };
static const char* const LED_LBL[3]  = { "вимк", "стан", "музика" };
static const char* vNightFrom(){ static char b[8]; snprintf(b, sizeof(b), "%02u:%02u", extras.s.nightFrom / 2, (extras.s.nightFrom % 2) * 30); return b; }
static const char* vNightTo(){ static char b[8]; snprintf(b, sizeof(b), "%02u:%02u", extras.s.nightTo / 2, (extras.s.nightTo % 2) * 30); return b; }
static const char* vBattery(){
  static char b[48];
  uint16_t mv = extras.batMv();
  if(extras.s.noBat)       snprintf(b, sizeof(b), "не показується");
  else if(mv == 0)         snprintf(b, sizeof(b), "вимірюю…");
  else if(mv < 2800)       snprintf(b, sizeof(b), "не знайдено");
  else if(extras.onUsb())  snprintf(b, sizeof(b), tr("USB, %u.%02u В"), mv / 1000, (mv % 1000) / 10);
  else                     snprintf(b, sizeof(b), tr("%d%%, %u.%02u В"), extras.batPct(), mv / 1000, (mv % 1000) / 10);
  return b;
}
static bool nightOn(){ return extras.s.nightOn; }

static Item s_scrItems[] = {
  iGap(4),
  iSlider("Яскравість", 5, 100, [](){ return (int32_t)config.store.brightness; },
          [](int32_t v){ config.store.brightness = (uint8_t)v; config.setBrightness(true); }, "%"),
  iSection("НІЧНИЙ РЕЖИМ"),
  iSwitch("Нічний режим", IC_MOON, C_VIOLET, [](){ return (int32_t)extras.s.nightOn; },
          [](int32_t v){ extras.s.nightOn = v; extras.changed(); }),
  iNav("Початок", IC_CLOCK, C_VIOLET, vNightFrom, [](){ M.push(&pgNightFrom); }),
  iNav("Кінець", IC_CLOCK, C_VIOLET, vNightTo, [](){ M.push(&pgNightTo); }),
  iSlider("Яскравість уночі", 0, 100, [](){ return (int32_t)extras.s.nightLevel; },
          [](int32_t v){ extras.s.nightLevel = (uint8_t)v; extras.changed(); }, "%"),
  iSection("БАТАРЕЯ"),
  iSeg("Без зарядника пригасити через", SAVE_LBL, 5, [](){ return (int32_t)extras.s.batSave; },
       [](int32_t v){ extras.s.batSave = (uint8_t)v; extras.changed(); }),
  iInfo("Стан батареї", vBattery),
  iSection("СВІТЛОДІОД НА ПЛАТІ"),
  iSeg("", LED_LBL, 3, [](){ return (int32_t)extras.s.ledMode; }, [](int32_t v){ extras.s.ledMode = (uint8_t)v; extras.changed(); }),
};
static ListPage s_screen("Екран", s_scrItems, sizeof(s_scrItems) / sizeof(s_scrItems[0]));
Page& pgScreen = s_screen;

/*  =================== вибір часу (ніч: з/до) =================== */
class HalfHourPage : public ListPage {
  public:
    HalfHourPage(const char* t, bool from, Item* items, uint8_t n) : ListPage(t, items, n), _from(from) {}
    void enter() override;
    void tick(uint32_t now) override;
    void draw(Gfx& g) override { ListPage::draw(g); drum.draw(g); }
    int16_t hit(int16_t x, int16_t y, Rect& r, uint8_t& radius) override {
      if(y >= drum.top && y < drum.top + drum.h){ r = Rect(MX, drum.top, CWID, drum.h); radius = R_CARD; return 100; }
      return ListPage::hit(x, y, r, radius);
    }
    uint8_t grab(int16_t id) override { return id == 100 ? 2 : ListPage::grab(id); }
    void drag(int16_t id, int16_t x, int16_t y, bool end) override {
      if(id != 100){ ListPage::drag(id, x, y, end); return; }
      if(!drum.drag && !end) drum.press(x, y);
      else if(!end) drum.move(y);
      else drum.release();
      M.inval(Rect(0, drum.top, SW, drum.h));
    }
    void value(int16_t id, int32_t v) override {
      if(id != 100){ ListPage::value(id, v); return; }
      uint8_t& f = _from ? extras.s.nightFrom : extras.s.nightTo;
      f = (uint8_t)v; extras.changed();
    }
    Drum drum;
  private:
    bool _from;
    int32_t _sent = -1;
};

static const char* hhLabel(uint8_t c, int16_t v){ static char b[4]; snprintf(b, sizeof(b), "%02d", c ? v * 30 : v); return b; }

void HalfHourPage::enter(){
  ListPage::enter();
  uint8_t hv = _from ? extras.s.nightFrom : extras.s.nightTo;
  drum.top = 10; drum.h = 118; drum.n[0] = 24; drum.n[1] = 2; drum.label = hhLabel;
  drum.drag = false;
  drum.setVal(0, hv / 2); drum.setVal(1, hv % 2);
  _sent = hv;
}

void HalfHourPage::tick(uint32_t now){
  ListPage::tick(now);
  if(drum.tick(now)) M.inval(Rect(0, drum.top, SW, drum.h));
  if(!drum.drag && !drum.snapping[0] && !drum.snapping[1]){
    int32_t v = drum.at(0) * 2 + drum.at(1);
    if(v != _sent){ _sent = v; M.postValue(100, v, true); }
    uint8_t cur = _from ? extras.s.nightFrom : extras.s.nightTo;
    if(cur != _sent && cur < 48){ _sent = cur; drum.setVal(0, cur / 2); drum.setVal(1, cur % 2); M.inval(Rect(0, drum.top, SW, drum.h)); }
  }
}

static Item s_nfItems[] = { iGap(136), iNote([](){ return "крутіть години й хвилини пальцем\nабо торкніться значення вище чи нижче"; }, 40) };
static Item s_ntItems[] = { iGap(136), iNote([](){ return "крутіть години й хвилини пальцем\nабо торкніться значення вище чи нижче"; }, 40) };
static HalfHourPage s_nightFrom("Нічний режим з", true, s_nfItems, 2);
static HalfHourPage s_nightTo("Нічний режим до", false, s_ntItems, 2);
Page& pgNightFrom = s_nightFrom;
Page& pgNightTo = s_nightTo;

/*  =================== «Будильник і сон» =================== */
static const char* const DAYS_LBL[2]  = { "щодня", "будні" };
static const char* const SLEEP_LBL[5] = { "вимк", "15", "30", "60", "90" };
static const char* vAlarmNote(){
  static char b[96];
  int32_t m = extras.alarmInMin();
  char st[48];
  if(config.getMode() == PM_WEB && config.station.name[0]) snprintf(st, sizeof(st), "%s", config.station.name);
  else snprintf(st, sizeof(st), "%s", tr("остання станція"));
  if(!extras.s.alarmOn) snprintf(b, sizeof(b), tr("вимкнено · заграє %s"), st);
  else if(m < 0)        snprintf(b, sizeof(b), tr("час ще не відомий · заграє %s"), st);
  else                  snprintf(b, sizeof(b), tr("через %d год %02d хв · заграє %s"), (int)(m / 60), (int)(m % 60), st);
  return b;
}
static const char* vSleepNote(){
  static char b[40];
  uint16_t sm = extras.sleepMinutes();
  if(!sm) snprintf(b, sizeof(b), "таймер вимкнено");
  else { uint32_t s = extras.sleepLeftSec(); snprintf(b, sizeof(b), tr("радіо замовкне через %u:%02u"), (unsigned)(s / 60), (unsigned)(s % 60)); }
  return b;
}

class AlarmPage : public HalfHourPage {
  public:
    AlarmPage(Item* items, uint8_t n) : HalfHourPage("Будильник і сон", true, items, n) {}
    void enter() override {
      ListPage::enter();
      drum.top = 4; drum.h = 118; drum.n[0] = 24; drum.n[1] = 12; drum.label = alLabel; drum.drag = false;
      drum.setVal(0, extras.s.alarmH); drum.setVal(1, extras.s.alarmM / 5);
      _asent = extras.s.alarmH * 60 + extras.s.alarmM;
    }
    void tick(uint32_t now) override {
      ListPage::tick(now);
      if(drum.tick(now)) M.inval(Rect(0, drum.top, SW, drum.h));
      if(!drum.drag && !drum.snapping[0] && !drum.snapping[1]){
        int32_t v = drum.at(0) * 60 + drum.at(1) * 5;
        if(v != _asent){ _asent = v; M.postValue(100, v, true); }
        int32_t cur = extras.s.alarmH * 60 + extras.s.alarmM;
        if(cur != _asent){ _asent = cur; drum.setVal(0, extras.s.alarmH); drum.setVal(1, extras.s.alarmM / 5); M.inval(Rect(0, drum.top, SW, drum.h)); }
      }
    }
    void value(int16_t id, int32_t v) override {
      if(id != 100){ ListPage::value(id, v); return; }
      extras.s.alarmH = (uint8_t)(v / 60); extras.s.alarmM = (uint8_t)(v % 60);
      extras.s.alarmOn = 1;                     /* виставили час — отже, будильник потрібен */
      extras.changed();
    }
  private:
    int32_t _asent = -1;
    static const char* alLabel(uint8_t c, int16_t v){ static char b[4]; snprintf(b, sizeof(b), "%02d", c ? v * 5 : v); return b; }
};

static Item s_alItems[] = {
  iGap(126),
  iSwitch("Будильник", IC_ALARM, C_ORANGE, [](){ return (int32_t)extras.s.alarmOn; }, [](int32_t v){ extras.s.alarmOn = v; extras.changed(); }),
  iSeg("", DAYS_LBL, 2, [](){ return (int32_t)extras.s.alarmDays; }, [](int32_t v){ extras.s.alarmDays = (uint8_t)v; extras.changed(); }),
  iNote(vAlarmNote, 24),
  iSection("ТАЙМЕР СНУ, ХВ"),
  iSeg("", SLEEP_LBL, 5, [](){ uint16_t m = extras.sleepMinutes(); for(uint8_t i = 0; i < 5; i++) if(SLEEP_MIN[i] == m) return (int32_t)i; return (int32_t)-1; },
       [](int32_t v){ if(v >= 0 && v < 5) extras.setSleep(SLEEP_MIN[v]); }),
  iNote(vSleepNote, 24),
};
static AlarmPage s_alarm(s_alItems, sizeof(s_alItems) / sizeof(s_alItems[0]));
Page& pgAlarm = s_alarm;

/*  =================== «Часовий пояс» =================== */
class TzPage : public HalfHourPage {
  public:
    TzPage(Item* items, uint8_t n) : HalfHourPage("Часовий пояс", true, items, n) {}
    void enter() override {
      ListPage::enter();
      drum.top = 10; drum.h = 118; drum.n[0] = 27; drum.n[1] = 4; drum.label = tzLabel; drum.drag = false;
      int8_t h = config.store.tzHour, m = config.store.tzMin;
      drum.setVal(0, h + 12); drum.setVal(1, abs(m) / 15);
      _tsent = (h + 12) * 4 + abs(m) / 15;
    }
    void tick(uint32_t now) override {
      ListPage::tick(now);
      if(drum.tick(now)) M.inval(Rect(0, drum.top, SW, drum.h));
      if(!drum.drag && !drum.snapping[0] && !drum.snapping[1]){
        int32_t v = drum.at(0) * 4 + drum.at(1);
        if(v != _tsent){ _tsent = v; M.postValue(100, v, true); }
      }
    }
    void value(int16_t id, int32_t v) override {
      if(id != 100){ ListPage::value(id, v); return; }
      int8_t h = (int8_t)(v / 4) - 12, m = (int8_t)((v % 4) * 15);
      config.setTimezone(h, m);
      config.setTimeConf();             /* пояс і літній час — у годинник, SNTP — заново */
      timekeeper.forceTimeSync = true;
    }
  private:
    int32_t _tsent = -1;
    static const char* tzLabel(uint8_t c, int16_t v){ static char b[6]; if(c) snprintf(b, sizeof(b), "%02d", v * 15); else snprintf(b, sizeof(b), "%+d", v - 12); return b; }
};
static const char* vNow(){ static char b[24]; strftime(b, sizeof(b), "%H:%M:%S", &network.timeinfo); return b; }
/*  Літній час (26.10.2026): пояс на барабані — зимовий, а влітку годинник сам додає годину.  */
static const char* vTzNote(){
  if(extras.s.dstMode != 1) return "години від Гринвіча (Київ: +2 взимку, +3 влітку)";
  time_t t = time(nullptr); struct tm lt; localtime_r(&t, &lt);
  return t > 1600000000 && lt.tm_isdst > 0 ? "пояс зимовий (Київ +2); зараз літній, +1 година"
                                           : "пояс зимовий (Київ +2); улітку додасться година";
}
static Item s_tzItems[] = { iGap(136), iInfo("Зараз", vNow),
  iSwitch("Літній час — сам", IC_SUN, C_ORANGE, [](){ return (int32_t)(extras.s.dstMode == 1); },
          [](int32_t v){ extras.s.dstMode = v ? 1 : 0; extras.changed(); config.applyTz(); }),
  iNote(vTzNote, 24) };
static TzPage s_tz(s_tzItems, 4);
Page& pgTz = s_tz;

/*  =================== «Обране» =================== */
class FavPage : public Page {
  public:
    const char* title() override { return "Обране"; }
    void enter() override;
    void leave() override;
    void draw(Gfx& g) override;
    void tick(uint32_t now) override;
    int16_t hit(int16_t x, int16_t y, Rect& r, uint8_t& radius) override;
    void tap(int16_t id, int16_t x, int16_t y) override;
    bool hold(int16_t id) override;
    int16_t height() override { return 4 + 2 * 78 + 50 + 8; }
  private:
    uint16_t* _logo[FAV_N] = { nullptr };
    bool _logoOk[FAV_N] = { false };
    volatile bool _reload = false;
    uint32_t _sig = 0, _sigT = 0;
    void _load();
    static Rect cell(uint8_t i){ int16_t w = (CWID - 16) / 3; return Rect(MX + (i % 3) * (w + 8), 4 + (i / 3) * 78, w, 72); }
};

static uint32_t crc32s(const char* s){
  uint32_t c = 0xFFFFFFFF;
  for(; *s; s++){ c ^= (uint8_t)*s; for(uint8_t k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & (0 - (c & 1))); }
  return ~c;
}

void FavPage::_load(){
  for(uint8_t i = 0; i < FAV_N; i++){
    _logoOk[i] = false;
    if(!extras.fav[i].url[0]) continue;
    if(!_logo[i]) _logo[i] = (uint16_t*)heap_caps_malloc(LOGO_S * LOGO_S * 2, MALLOC_CAP_SPIRAM);
    if(!_logo[i]) continue;
    char path[28]; snprintf(path, sizeof(path), "/logo/%08x.565", (unsigned)crc32s(extras.fav[i].url));
    if(!SPIFFS.exists(path)) continue;
    File f = SPIFFS.open(path, "r");
    if(f && f.size() == LOGO_S * LOGO_S * 2) _logoOk[i] = f.read((uint8_t*)_logo[i], LOGO_S * LOGO_S * 2) == LOGO_S * LOGO_S * 2;
    if(f) f.close();
  }
}

void FavPage::enter(){ _load(); _sig = 0; }
void FavPage::leave(){ for(uint8_t i = 0; i < FAV_N; i++){ _logoOk[i] = false; } }

void FavPage::draw(Gfx& g){
  int8_t playing = extras.favPlaying();
  for(uint8_t i = 0; i < FAV_N; i++){
    Rect r = cell(i);
    if(!g.visible(r.x, r.y, r.w, r.h)) continue;
    const FavItem& f = extras.fav[i];
    if(!f.url[0]){
      g.frame(r.x, r.y, r.w, r.h, 14, C_LINE, 1);
      icon(g, IC_PLUS, r.x + r.w / 2, r.y + 28, C_TXT2, C_BG);
      g.text(r.x + r.w / 2, r.y + 56, "додати", F_SM, C_TXT2, AL_C);
      continue;
    }
    bool on = playing == (int8_t)i;
    g.box(r.x, r.y, r.w, r.h, 14, C_SURF);
    if(on) g.frame(r.x, r.y, r.w, r.h, 14, C_ACC, 2);
    const int16_t ls = 34, lx = r.x + 9, ly = r.y + 9;
    if(_logoOk[i] && _logo[i]){
      /*  логотип 45×45 зменшуємо до 34×34 найближчим сусідом  */
      static uint16_t* small = (uint16_t*)heap_caps_malloc(34 * 34 * 2, MALLOC_CAP_SPIRAM);
      if(!small) continue;
      for(int16_t yy = 0; yy < ls; yy++) for(int16_t xx = 0; xx < ls; xx++)
        small[yy * ls + xx] = _logo[i][(yy * LOGO_S / ls) * LOGO_S + xx * LOGO_S / ls];
      g.image(lx, ly, ls, ls, small, 8);
    }else{
      static const uint16_t PAL[8] = { 0x3A8D, 0x5A4B, 0x2C6A, 0x6A28, 0x2B0F, 0x7A6C, 0x4B09, 0x31CC };
      uint16_t c = PAL[crc32s(f.url) & 7];
      g.box(lx, ly, ls, ls, 8, c);
      char ini[8] = { 0 };
      /*  дві перші літери назви (UTF-8)  */
      const char* s = f.name; uint8_t k = 0, chars = 0;
      while(*s && chars < 2 && k < 6){ uint8_t b = (uint8_t)*s; uint8_t len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 1; if(b == ' '){ s++; continue; } for(uint8_t j = 0; j < len && s[j]; j++) ini[k++] = s[j]; s += len; chars++; }
      g.text(lx + ls / 2, ly + 22, ini, F_ROWB, 0xFFFF, AL_C);
    }
    if(on){
      /*  «грає» — три риски  */
      static const uint8_t H[3] = { 9, 14, 6 };
      for(uint8_t k = 0; k < 3; k++) g.line(r.x + r.w - 22 + k * 5, r.y + 26, r.x + r.w - 22 + k * 5, r.y + 26 - H[k], 2.6f, C_ACC);
    }
    g.text(r.x + 9, r.y + 62, f.name, F_SMB, on ? C_ACC : C_TXT, AL_L, r.w - 16);
  }
  /*  рядок обраного на головному  */
  int16_t y = 4 + 2 * 78 + 2;
  if(g.visible(MX, y, CWID, 44)){
    g.box(MX, y, CWID, 44, R_CARD, C_SURF);
    g.text(MX + 14, y + 26, "Показувати на головному", F_ROW, C_TXT, AL_L, CWID - 80);
    drawSwitch(g, MX + CWID - 52, y + 10, extras.s.favHide ? 0 : 1);
  }
  g.text(SW / 2, y + 62, "торкніться порожньої — додати станцію, що грає; утримайте — прибрати", F_SM, C_TXT2, AL_C, CWID);
}

void FavPage::tick(uint32_t now){
  if(now - _sigT < 300) return;
  _sigT = now;
  uint32_t s = (uint32_t)extras.favPlaying() + 7 + (extras.s.favHide ? 1000 : 0);
  for(uint8_t i = 0; i < FAV_N; i++){ s = s * 31 + (uint8_t)extras.fav[i].url[0] + (_logoOk[i] ? 3 : 0); s = s * 31 + crc32s(extras.fav[i].name); }
  if(s != _sig){ _sig = s; M.inval(Rect(0, 0, SW, height())); }
}

int16_t FavPage::hit(int16_t x, int16_t y, Rect& r, uint8_t& radius){
  for(uint8_t i = 0; i < FAV_N; i++){ Rect c = cell(i); if(c.has(x, y)){ r = c; radius = 14; return i; } }
  int16_t yy = 4 + 2 * 78 + 2;
  if(y >= yy && y < yy + 44){ r = Rect(MX, yy, CWID, 44); radius = R_CARD; return 10; }
  return -1;
}

void FavPage::tap(int16_t id, int16_t x, int16_t y){
  (void)x; (void)y;
  if(id == 10){ extras.s.favHide = !extras.s.favHide; extras.changed(); return; }
  if(id < 0 || id >= FAV_N) return;
  if(extras.fav[id].url[0]){
    if(extras.favPlay(id)) M.close();
    else M.toast("цієї станції вже нема в списку");
  }else{
    if(extras.favSetCurrent(id)){ M.toast("додано в обране"); _load(); }
    else M.toast("спершу увімкніть радіостанцію");
  }
}

bool FavPage::hold(int16_t id){
  if(id < 0 || id >= FAV_N || !extras.fav[id].url[0]) return false;
  extras.favClear(id);
  M.toast("прибрано з обраного");
  return true;
}

static FavPage s_fav;
Page& pgFav = s_fav;

/*  =================== «Проповіді» =================== */
class SermPage : public Page {
  public:
    const char* title() override { return "Проповіді"; }
    void enter() override { _ver = 0xFFFFFFFF; _jump = true; }
    int16_t height() override { uint16_t n = sermons.count(); return n ? 6 + n * RH + 10 : CH; }
    void draw(Gfx& g) override;
    void tick(uint32_t now) override;
    int16_t hit(int16_t x, int16_t y, Rect& r, uint8_t& radius) override;
    void tap(int16_t id, int16_t x, int16_t y) override;
  private:
    static const int16_t RH = 52;
    uint32_t _ver = 0; bool _jump = true; uint16_t _got = 0; bool _load = false; int16_t _play = -2;
};

void SermPage::draw(Gfx& g){
  uint16_t n = sermons.count();
  if(!n){
    g.box(MX, 40, CWID, 110, R_CARD, C_SURF);
    icon(g, IC_CROSS, SW / 2, 72, C_VIOLET, C_SURF);
    char b[48];
    if(sermons.loading()){
      if(sermons.loadedSoFar()) snprintf(b, sizeof(b), tr("завантажую з сайту… %u"), sermons.loadedSoFar());
      else snprintf(b, sizeof(b), "завантажую з сайту…");
      g.text(SW / 2, 110, b, F_ROW, C_TXT, AL_C, CWID - 20);
    }else{
      g.text(SW / 2, 106, sermons.error()[0] ? sermons.error() : "список порожній", F_ROW, C_TXT, AL_C, CWID - 20);
      g.text(SW / 2, 128, "торкніться, щоб завантажити", F_SM, C_ACC, AL_C);
    }
    return;
  }
  int16_t first = (-g.oy() + HDR) / RH - 1; if(first < 0) first = 0;
  int16_t play = sermons.playing();
  for(int16_t i = first; i < (int16_t)n && i < first + 7; i++){
    int16_t y = 6 + i * RH;
    if(!g.visible(MX, y, CWID, RH)) continue;
    const Sermon* s = sermons.at(i);
    if(!s) continue;
    bool on = i == play;
    drawCard(g, MX, y, CWID, RH, i == 0, i == (int16_t)n - 1, C_SURF);
    if(i) g.fill(MX + 50, y, CWID - 50, 1, C_LINE);
    g.box(MX + 12, y + 12, 28, 28, R_BADGE, on ? C_ACC : C_SURF2);
    if(on) icon(g, IC_WAVE, MX + 26, y + 26, C_ACCTXT, C_ACC);
    else icon(g, IC_PLAY, MX + 27, y + 26, C_TXT2, C_SURF2);
    g.text(MX + 50, y + 22, s->title, F_ROW, on ? C_ACC : C_TXT, AL_L, CWID - 62);
    char b[96];
    if(s->dur) snprintf(b, sizeof(b), tr("%s · %s · %u хв"), s->preacher, s->date, (unsigned)((s->dur + 30) / 60));
    else snprintf(b, sizeof(b), "%s · %s", s->preacher, s->date);
    g.text(MX + 50, y + 40, b, F_SM, C_TXT2, AL_L, CWID - 62);
  }
}

void SermPage::tick(uint32_t now){
  (void)now;
  uint32_t v = sermons.version();
  bool ld = sermons.loading();
  uint16_t got = sermons.loadedSoFar();
  int16_t pl = sermons.playing();
  if(v != _ver || ld != _load || got != _got || pl != _play){
    bool fresh = v != _ver;
    _ver = v; _load = ld; _got = got; _play = pl;
    if(fresh && _jump && pl >= 0 && sermons.count()){
      /*  одразу до тієї, що грає  */
      int16_t s = 6 + pl * RH - CH / 2 + RH / 2;
      int16_t mx = height() - CH; if(s > mx) s = mx; if(s < 0) s = 0;
      scroll = s; _jump = false;
    }
    M.invalAll();
  }
}

int16_t SermPage::hit(int16_t x, int16_t y, Rect& r, uint8_t& radius){
  (void)x;
  uint16_t n = sermons.count();
  if(!n){ if(!sermons.loading() && y >= 40 && y < 150){ r = Rect(MX, 40, CWID, 110); radius = R_CARD; return 9999; } return -1; }
  int16_t i = (y - 6) / RH;
  if(y < 6 || i < 0 || i >= (int16_t)n) return -1;
  r = Rect(MX, 6 + i * RH, CWID, RH); radius = (i == 0 || i == (int16_t)n - 1) ? R_CARD : 0;
  return i;
}

void SermPage::tap(int16_t id, int16_t x, int16_t y){
  (void)x; (void)y;
  if(id == 9999){ sermons.fetch(); return; }
  if(id >= 0 && sermons.count() && sermons.play(id)) M.close();
}

static SermPage s_serm;
Page& pgSermons = s_serm;

/*  =================== «Про радіо» =================== */
static const char* vBuild(){ static char b[40]; snprintf(b, sizeof(b), "%s, %s", prVersion(), prBuild()); return b; }
static const char* vNet(){ return WB::staUp() ? WB::curSsid() : "немає"; }
static const char* vIp(){ return WB::staUp() ? WB::ip() : "-"; }
static const char* vRssi(){ static char b[16]; if(WB::staUp()) snprintf(b, sizeof(b), "%d dBm", (int)WB::rssi()); else snprintf(b, sizeof(b), "-"); return b; }
static const char* vStream(){ static char b[40]; if(player.status() == PLAYING && config.station.bitrate) snprintf(b, sizeof(b), tr("%d кбіт/с, %s"), config.station.bitrate, player.getCodecname()); else snprintf(b, sizeof(b), "-"); return b; }
static const char* vWeather(){ static char b[40]; if(timekeeper.weatherHave) snprintf(b, sizeof(b), tr("%.1f°  %d мм  %d%%"), (float)timekeeper.weatherTemp, (int)timekeeper.weatherPress, (int)timekeeper.weatherHum); else snprintf(b, sizeof(b), "-"); return b; }
static const char* vBat2(){
  static char b[48];
  uint16_t mv = extras.batMv();
  if(extras.s.noBat) snprintf(b, sizeof(b), "не показується");
  else if(mv < 2800) snprintf(b, sizeof(b), "-");
  else snprintf(b, sizeof(b), tr("%d%%, %u.%02u В%s"), extras.batPct(), mv / 1000, (mv % 1000) / 10, extras.charged() ? tr(", заряджено") : (extras.charging() ? tr(", заряджається") : ""));
  return b;
}
static const char* vHeap(){ static char b[24]; snprintf(b, sizeof(b), tr("%u КБ"), (unsigned)(ESP.getFreeHeap() / 1024)); return b; }
static Item s_infoItems[] = {
  iSection("ПОТУЖНЕ РАДІО"),
  iInfo("Версія", vBuild),
  iSection("МЕРЕЖА"),
  iInfo("Мережа", vNet), iInfo("Адреса", vIp), iInfo("Сигнал", vRssi),
  iSection("ЗАРАЗ"),
  iInfo("Потік", vStream), iInfo("Погода", vWeather), iInfo("Батарея", vBat2), iInfo("Вільна пам'ять", vHeap),
};
static ListPage s_info("Про радіо", s_infoItems, sizeof(s_infoItems) / sizeof(s_infoItems[0]));
Page& pgInfo = s_info;

/*  =================== «Живлення» =================== */
/*  Утримати кнопку, поки коло не замкнеться, — випадково не вимкнеш.  */
class PowerPage : public Page {
  public:
    const char* title() override { return "Живлення"; }
    void enter() override { _arm = -1; _go = -1; _frames = 0; _done = false; }
    bool scrollable() override { return false; }
    void draw(Gfx& g) override;
    void tick(uint32_t now) override;
    int16_t hit(int16_t x, int16_t y, Rect& r, uint8_t& radius) override;
    uint8_t grab(int16_t id) override { return id >= 0 ? 2 : 0; }
    void drag(int16_t id, int16_t x, int16_t y, bool end) override;
  private:
    volatile int8_t _arm = -1, _go = -1;
    uint32_t _armT = 0;
    uint8_t _frames = 0;
    bool _done = false;
    static Rect btn(uint8_t i){ return Rect(MX, 8 + i * 84, CWID, 76); }
};

void PowerPage::draw(Gfx& g){
  static const char* T1[2] = { "Перезавантажити", "Вимкнути" };
  static const char* T2[2] = { "звук стихне на 10 секунд", "увімкнеться дотиком до екрана" };
  for(uint8_t i = 0; i < 2; i++){
    Rect r = btn(i);
    if(!g.visible(r.x, r.y, r.w, r.h)) continue;
    bool arm = _arm == (int8_t)i;
    g.box(r.x, r.y, r.w, r.h, 16, C_SURF);
    float cx = r.x + 40, cy = r.y + r.h / 2.0f;
    uint16_t col = i ? C_RED : C_ORANGE;
    g.circle(cx, cy, 24, arm ? Gfx::blend(C_SURF, col, 60) : C_SURF2);
    if(arm){
      float k = (millis() - _armT) / 900.0f; if(k > 1) k = 1;
      if(k > 0.01f) g.arc(cx, cy, 24, 4, col, 0, 360 * k);
    }
    icon(g, i ? IC_POWER : IC_RESTART, cx, cy, col, C_SURF2);
    g.text(r.x + 76, r.y + 34, _go == (int8_t)i ? (i ? "Вимикаюсь…" : "Перезавантажую…") : T1[i], F_ROWB, C_TXT, AL_L, r.w - 90);
    const char* sub = T2[i];
    char ab[64];
    if(i == 1 && extras.s.alarmOn){ snprintf(ab, sizeof(ab), tr("дотиком або будильником о %02u:%02u"), extras.s.alarmH, extras.s.alarmM); sub = ab; }
    g.text(r.x + 76, r.y + 54, arm ? "тримайте, поки коло не замкнеться" : sub, F_SM, arm ? col : C_TXT2, AL_L, r.w - 90);
  }
  g.text(SW / 2, 186, "утримайте кнопку ~1 секунду", F_SM, C_TXT2, AL_C);
}

void PowerPage::tick(uint32_t now){
  if(_arm >= 0 && _go < 0){
    M.inval(Rect(0, btn(_arm).y, SW, btn(_arm).h));
    if(now - _armT >= 900){ _go = _arm; M.invalAll(); }
  }
  /*  напис «вимикаюсь» уже на екрані — виконуємо (тут, у задачі дисплея: екран спить лише звідси)  */
  if(_go >= 0 && !_done && ++_frames > 3){
    _done = true;
    if(_go == 1){ delay(900); extras.pwmSet(0); display.deepsleep(); }
    extras.requestPower(_go == 0 ? 1 : 2);
  }
}

int16_t PowerPage::hit(int16_t x, int16_t y, Rect& r, uint8_t& radius){
  for(uint8_t i = 0; i < 2; i++){ Rect b = btn(i); if(b.has(x, y)){ r = b; radius = 16; return i; } }
  return -1;
}

void PowerPage::drag(int16_t id, int16_t x, int16_t y, bool end){
  (void)x; (void)y;
  if(_go >= 0) return;
  if(end){ if(_arm >= 0){ int8_t a = _arm; _arm = -1; M.inval(Rect(0, btn(a).y, SW, btn(a).h)); } return; }
  if(_arm != id){ _arm = id; _armT = millis(); }
}

static PowerPage s_power;
Page& pgPower = s_power;

}  // namespace m2
