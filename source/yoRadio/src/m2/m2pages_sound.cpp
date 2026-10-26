/*  Нове меню: «Еквалайзер», «Обробка», «Під кімнату», «Мікрофон»,
    «Хлопки й стук», «Присутність».  */
#include "../core/options.h"
#include "m2pages.h"
#include "m2lang.h"
#include "../core/config.h"
#include "../extras/yoExtras.h"
#include "../extras/yoDsp.h"
#include "../extras/yoMic.h"

namespace m2 {

/*  =================== «Еквалайзер» =================== */
static const char* const EQ_FRQ[10] = { "31", "62", "125", "250", "500", "1к", "2к", "4к", "8к", "16к" };

class EqPage : public Page {
  public:
    const char* title() override { return "Еквалайзер"; }
    void enter() override { _sig = 0; for(uint8_t b = 0; b < 10; b++){ _anim[b] = extras.s.eq[b]; } _drag = -1; }
    bool scrollable() override { return false; }
    void draw(Gfx& g) override;
    void tick(uint32_t now) override;
    void loop(uint32_t now) override;
    int16_t hit(int16_t x, int16_t y, Rect& r, uint8_t& radius) override;
    uint8_t grab(int16_t id) override { return (id >= 100 && id < 110) ? 2 : 0; }
    void drag(int16_t id, int16_t x, int16_t y, bool end) override;
    void tap(int16_t id, int16_t x, int16_t y) override;
    void value(int16_t id, int32_t v) override;
    /*  службове (консоль «items»): геометрія смуг для автоперевірки  */
    void dump() override {
      Serial.printf("EQGEO\t%d\t%d\t%d\t%d\n", (int)CY0, (int)CHH, (int)zeroY(), (int)(T1 - T0));
      for(uint8_t b = 0; b < 10; b++)
        Serial.printf("BAND\t%u\t%d\t%d\t%d\n", (unsigned)b, (int)colX(b), (int)extras.s.eq[b], (int)lroundf(yOf(extras.s.eq[b])));
    }
  private:
    static const int16_t CY0 = 62, CHH = 110, T0 = 76, T1 = 150;
    float _anim[10] = { 0 };
    int8_t _drag = -1; int8_t _dragV = 0; uint32_t _dragHold = 0;
    uint32_t _sig = 0, _sigT = 0, _applyT = 0;
    bool _pend = false;
    static Rect chip(uint8_t i){ int16_t w = (CWID - 24) / 4; return Rect(MX + (i % 4) * (w + 8), 4 + (i / 4) * 28, w, 24); }
    static Rect btn(uint8_t i){ int16_t w = (CWID - 16) / 3; return Rect(MX + i * (w + 8), 176, w, 24); }
    static int16_t colX(uint8_t b){ return MX + 6 + (CWID - 12) * b / 10 + (CWID - 12) / 20; }
    static int16_t zeroY(){ return (T0 + T1) / 2; }
    static float yOf(float db){ return zeroY() - db * (T1 - T0) / 24.0f; }
};

void EqPage::draw(Gfx& g){
  const ExtStore& e = extras.s;
  for(uint8_t i = 0; i < EQ_PRESETS; i++){
    Rect r = chip(i);
    if(!g.visible(r.x, r.y, r.w, r.h)) continue;
    bool on = e.eqOn && e.eqPreset == i;
    g.box(r.x, r.y, r.w, r.h, 12, on ? C_ACC : C_SURF);
    g.text(r.x + r.w / 2, r.y + 16, YoDsp::PRESET_NAME[i], F_SMB, on ? C_ACCTXT : (e.eqOn ? C_TXT : C_TXT2), AL_C, r.w - 6);
  }
  if(g.visible(MX, CY0, CWID, CHH)){
    g.box(MX, CY0, CWID, CHH, R_CARD, C_SURF);
    const int16_t zy = zeroY();
    g.fill(MX + 10, zy, CWID - 20, 1, C_LINE);
    for(uint8_t b = 0; b < 10; b++){
      const int16_t cx = colX(b);
      g.box(cx - 2, T0, 4, T1 - T0, 2, C_SURF2);
      float v = _anim[b];
      float yv = yOf(v);
      uint16_t ac = e.eqOn ? C_ACC : C_TXT3;
      if(fabsf(v) > 0.2f){
        float a = yv < zy ? yv : zy, bb = yv < zy ? zy : yv;
        g.box(cx - 2, (int16_t)a, 4, (int16_t)(bb - a) + 1, 2, ac);
      }
      if(e.eqRoomOn && e.eqRoom[b]) g.circle(cx + 7, yOf((e.eqOn ? v : 0) + e.eqRoom[b]), 2.3f, C_TEAL);
      g.circle(cx, yv, _drag == b ? 8.5f : 7, e.eqOn ? C_KNOB : C_TXT2);
      g.text(cx, CY0 + CHH - 6, EQ_FRQ[b], F_TINY, C_TXT2, AL_C);
      if(_drag == b){
        char t[6]; snprintf(t, sizeof(t), "%+d", (int)lroundf(v));
        g.text(cx, (int16_t)yv - 12, t, F_SMB, C_ACC, AL_C);
      }
    }
  }
  static const char* B[3] = { nullptr, "Під кімнату", "Обробка" };
  for(uint8_t i = 0; i < 3; i++){
    Rect r = btn(i);
    if(!g.visible(r.x, r.y, r.w, r.h)) continue;
    bool on = i == 0 && e.eqOn;
    g.box(r.x, r.y, r.w, r.h, 12, on ? C_ACC : C_SURF2);
    const char* t = i == 0 ? (e.eqOn ? "Увімкнено" : "Вимкнено") : B[i];
    g.text(r.x + r.w / 2, r.y + 16, t, F_SMB, on ? C_ACCTXT : C_TXT, AL_C, r.w - 8);
  }
}

void EqPage::tick(uint32_t now){
  const ExtStore& e = extras.s;
  if(now - _sigT >= 120){
    _sigT = now;
    uint32_t s = e.eqOn * 3 + e.eqPreset * 7 + e.eqRoomOn * 11;
    for(uint8_t b = 0; b < 10; b++) s = s * 31 + (uint8_t)e.eqRoom[b];
    if(s != _sig){ _sig = s; M.invalAll(); }
  }
  for(uint8_t b = 0; b < 10; b++){
    float t = (_drag == b || (_dragHold > now && _drag < 0 && false)) ? _dragV : e.eq[b];
    if(_drag == b) t = _dragV;
    float d = t - _anim[b];
    if(fabsf(d) > 0.01f){
      _anim[b] = fabsf(d) < 0.05f ? t : _anim[b] + d * 0.35f;
      M.inval(Rect(colX(b) - 16, CY0, 32, CHH));
    }
  }
}

void EqPage::loop(uint32_t now){
  if(_pend && now - _applyT >= 150){ _pend = false; _applyT = now; extras.changed(); yoDsp.changed(); }
}

int16_t EqPage::hit(int16_t x, int16_t y, Rect& r, uint8_t& radius){
  for(uint8_t i = 0; i < EQ_PRESETS; i++){ Rect c = chip(i); if(c.has(x, y)){ r = c; radius = 12; return i; } }
  for(uint8_t i = 0; i < 3; i++){ Rect c = btn(i); if(c.has(x, y)){ r = c; radius = 12; return 20 + i; } }
  if(y >= CY0 && y < CY0 + CHH){
    int b = (x - MX - 6) * 10 / (CWID - 12);
    if(b < 0) b = 0; if(b > 9) b = 9;
    r = Rect(colX(b) - 14, CY0, 28, CHH); radius = 0;
    return 100 + b;
  }
  return -1;
}

void EqPage::drag(int16_t id, int16_t x, int16_t y, bool end){
  (void)x;
  if(id < 100 || id >= 110) return;
  uint8_t b = id - 100;
  int db = (int)lroundf((zeroY() - y) * 24.0f / (T1 - T0));
  if(db > 12) db = 12; if(db < -12) db = -12;
  if(end){ _drag = -1; M.inval(Rect(colX(b) - 16, CY0, 32, CHH)); M.postValue(id, db, true); return; }
  if(_drag != b || _dragV != db){ _drag = b; _dragV = db; M.inval(Rect(colX(b) - 16, CY0, 32, CHH)); M.postValue(id, db, false); }
}

void EqPage::value(int16_t id, int32_t v){
  if(id < 100 || id >= 110) return;
  ExtStore& e = extras.s;
  uint8_t b = id - 100;
  if(e.eqPreset){ e.eqPreset = 0; }
  if(!e.eqOn) e.eqOn = 1;
  e.eq[b] = (int8_t)v;
  _pend = true;
}

void EqPage::tap(int16_t id, int16_t x, int16_t y){
  (void)x; (void)y;
  ExtStore& e = extras.s;
  if(id >= 0 && id < EQ_PRESETS){ yoDsp.applyPreset(id); return; }
  if(id == 20){ e.eqOn = !e.eqOn; extras.changed(); yoDsp.changed(); return; }
  if(id == 21){ M.push(&pgRoom); return; }
  if(id == 22){ M.push(&pgSound); return; }
}

static EqPage s_eq;
Page& pgEq = s_eq;

/*  =================== «Обробка» =================== */
static const char* const GUARD_LBL[3] = { "вимк", "м'який", "сильний" };
static const char* const VB_LBL[4]    = { "вимк", "1", "2", "3" };
static const char* const LOUD_LBL[3]  = { "вимк", "м'яка", "сильна" };
static void sndChanged(){ extras.changed(); yoDsp.changed(); }
/*  Захист динаміка діє лише там, де динамік: вбудований чи MAX98357A. З ЦАП на лінію
    перемикач нічого не міняв — власник 26.10.2026: «функції обробки не працюють». Кажемо прямо.  */
static const char* vGuardNote(){
  return (extras.s.dac == 0 || extras.s.dac == 3) ? "зрізає низ, який динамік не грає, а хрипить"
                                                  : "ЦАП на лінію: захист не потрібен";
}
static Item s_sndItems[] = {
  iSection("ДИНАМІК"),
  iSeg("Захист динаміка", GUARD_LBL, 3, [](){ return (int32_t)extras.s.eqGuard; }, [](int32_t v){ extras.s.eqGuard = v; sndChanged(); }),
  iNote(vGuardNote, 22),
  iSeg("Віртуальний бас", VB_LBL, 4, [](){ return (int32_t)extras.s.vbass; }, [](int32_t v){ extras.s.vbass = v; sndChanged(); }),
  iSeg("Тонкомпенсація, коли тихо", LOUD_LBL, 3, [](){ return (int32_t)extras.s.eqLoud; }, [](int32_t v){ extras.s.eqLoud = v; sndChanged(); }),
  iSection("СТЕРЕО"),
  iSlider("Баланс", -16, 16, [](){ return (int32_t)config.store.balance; }, [](int32_t v){ config.setBalance((int8_t)v); }),
};
static ListPage s_sound("Обробка", s_sndItems, sizeof(s_sndItems) / sizeof(s_sndItems[0]));
Page& pgSound = s_sound;

/*  =================== «Під кімнату» =================== */
static char s_rmErr[48] = { 0 };
static bool roomHas(){ for(uint8_t b = 0; b < 10; b++) if(extras.s.eqRoom[b]) return true; return false; }
static bool roomBusy(){ uint8_t st = yoDsp.roomState(); return st == 1 || st == 2; }

static void roomChart(Gfx& g, int16_t x, int16_t y, int16_t w, int16_t h){
  g.box(x, y, w, h, R_CARD, C_SURF);
  const ExtStore& e = extras.s;
  const int16_t zy = y + 56;
  g.fill(x + 10, zy, w - 20, 1, C_LINE);
  bool has = roomHas();
  for(uint8_t b = 0; b < 10; b++){
    const int16_t cx = x + 6 + (w - 12) * b / 10 + (w - 12) / 20;
    int v = e.eqRoom[b];
    if(v){
      int16_t yv = zy - v * 4;
      int16_t a = yv < zy ? yv : zy, bb = yv < zy ? zy : yv;
      g.box(cx - 5, a, 10, bb - a + 1, 3, e.eqRoomOn ? C_TEAL : C_TXT3);
    }
    g.text(cx, y + h - 8, EQ_FRQ[b], F_TINY, C_TXT2, AL_C);
  }
  if(!has) g.text(x + w / 2, zy - 12, "ще не міряли", F_SM, C_TXT2, AL_C);
  if(roomBusy()){
    uint8_t pr = yoDsp.roomProgress();
    g.box(x + 12, y + 8, w - 24, 4, 2, C_SURF2);
    if(pr) g.box(x + 12, y + 8, (int16_t)((w - 24) * pr / 100) < 4 ? 4 : (int16_t)((w - 24) * pr / 100), 4, 2, C_ACC);
  }
}
static const char* vRoomNote(){
  static char b[96];
  uint8_t st = yoDsp.roomState();
  if(s_rmErr[0])                 snprintf(b, sizeof(b), "%s", s_rmErr);
  else if(st == 1 || st == 2)    snprintf(b, sizeof(b), "%s… %u%%", yoDsp.roomMsg(), (unsigned)yoDsp.roomProgress());
  else if(st == 3)               snprintf(b, sizeof(b), "%s", yoDsp.roomMsg());
  else if(st == 4)               snprintf(b, sizeof(b), tr("не вийшло: %s"), yoDsp.roomMsg());
  else                           snprintf(b, sizeof(b), "радіо грає тони й слухає себе мікрофоном;\nу кімнаті має бути тихо");
  return b;
}
static const char* vRoomBtn(){ return roomBusy() ? "Зупинити" : (roomHas() ? "Зміряти ще раз" : "Зміряти"); }
static Item s_roomItems[] = {
  iGap(4),
  iCustom(130, roomChart),
  iNote(vRoomNote, 42),
  iButton("Зміряти", IC_WAVE, [](){
    ExtStore& e = extras.s;
    s_rmErr[0] = 0;
    if(roomBusy()){ mic.sweepAbort(); return; }
    if(!e.micOn){ e.micOn = 1; extras.changed(); mic.apply(); }
    const char* w = yoDsp.roomTuneStart();
    if(w) snprintf(s_rmErr, sizeof(s_rmErr), "%s", w);
  }),
  iSwitch("Поправка увімкнена", IC_ROOM, C_TEAL, [](){ return (int32_t)extras.s.eqRoomOn; },
          [](int32_t v){ if(!roomHas()){ snprintf(s_rmErr, sizeof(s_rmErr), "спершу зміряйте"); return; } extras.s.eqRoomOn = v; extras.changed(); yoDsp.changed(); }),
};
class RoomPage : public ListPage {
  public:
    RoomPage() : ListPage("Під кімнату", s_roomItems, sizeof(s_roomItems) / sizeof(s_roomItems[0])) {}
    void enter() override { s_rmErr[0] = 0; s_roomItems[3].text = vRoomBtn; ListPage::enter(); }
    void tick(uint32_t now) override {
      ListPage::tick(now);
      static uint32_t t = 0; static uint8_t st = 255, pr = 255;
      if(now - t >= 200){
        t = now;
        uint8_t s = yoDsp.roomState(), p = yoDsp.roomProgress();
        if(s != st || p != pr){ st = s; pr = p; M.inval(Rect(0, 0, SW, 180)); }
      }
    }
    bool keepOpen() override { return roomBusy(); }
};
static RoomPage s_room;
Page& pgRoom = s_room;

/*  =================== «Мікрофон» =================== */
static const char* const GAIN_LBL[4] = { "низька", "середня", "висока", "макс" };
static const uint8_t     GAIN_VAL[4] = { 3, 0, 7, 8 };
static void micMeter(Gfx& g, int16_t x, int16_t y, int16_t w, int16_t h){
  const int16_t mx = x + 14, mw = w - 28, my = y + 8;
  g.box(mx, my, mw, 10, 5, C_SURF2);
  if(mic.listening()){
    float lv = mic.levelDb(), nz = mic.noiseDb();
    int16_t lw = (int16_t)((lv + 80.0f) * mw / 60.0f); if(lw < 0) lw = 0; if(lw > mw) lw = mw;
    if(lw >= 10) g.box(mx, my, lw, 10, 5, mic.speech() ? C_GREEN : C_ACC);
    int16_t nx = (int16_t)((nz + 80.0f) * mw / 60.0f);
    if(nx >= 0 && nx < mw - 1) g.box(mx + nx - 1, my - 3, 3, 16, 1, C_TXT);
  }
  char t[72];
  uint32_t now = millis();
  if(!extras.s.micOn)                                   snprintf(t, sizeof(t), "мікрофон вимкнено");
  else if(mic.heard() && now - mic.heardMs() < 8000)    snprintf(t, sizeof(t), tr("почуто: %s — %s"), tr(YoMic::gestureName(mic.heard())), tr(YoMic::actionName(YoMic::actionFor((MicGesture)mic.heard()))));
  else if(mic.speech())                                 snprintf(t, sizeof(t), "чую голос");
  else if(mic.aecActive())                              snprintf(t, sizeof(t), "віднімаю власний звук радіо");
  else                                                  snprintf(t, sizeof(t), "слухаю");
  g.text(x + 14, y + h - 9, t, F_SM, C_TXT2, AL_L, w - 28);
}
static Item s_micItems[] = {
  iGap(4),
  iSwitch("Слухати", IC_MIC, C_VIOLET, [](){ return (int32_t)extras.s.micOn; }, [](int32_t v){ extras.s.micOn = v; extras.changed(); mic.apply(); }),
  [](){ Item i = iCustom(40, micMeter); i.color = 1; return i; }(),
  iSeg("Чутливість", GAIN_LBL, 4, [](){ for(uint8_t i = 0; i < 4; i++) if(GAIN_VAL[i] == extras.s.micGain) return (int32_t)i; return (int32_t)1; },
       [](int32_t v){ extras.s.micGain = GAIN_VAL[v]; extras.changed(); mic.apply(); }),
  iSwitch("Слухати й під час звуку", IC_SPEAKER, C_VIOLET, [](){ return (int32_t)extras.s.micPlay; }, [](int32_t v){ extras.s.micPlay = v; extras.changed(); }),
  iSection("ЩО ВМІЄ"),
  iNav("Хлопки й стук", IC_HAND, C_VIOLET, nullptr, [](){ M.push(&pgGest); }),
  iNav("Присутність", IC_PERSON, C_VIOLET, nullptr, [](){ M.push(&pgPres); }),
};
class MicPage : public ListPage {
  public:
    MicPage() : ListPage("Мікрофон", s_micItems, sizeof(s_micItems) / sizeof(s_micItems[0])) {}
    void tick(uint32_t now) override {
      ListPage::tick(now);
      static uint32_t t = 0;
      if(now - t >= 150){ t = now; const Item& it = s_micItems[2]; M.inval(Rect(0, it.y, SW, it.hh)); }
    }
};
static MicPage s_mic;
Page& pgMic = s_mic;

/*  =================== «Хлопки й стук» =================== */
static const char* const GTAB_LBL[2] = { "хлопки", "стук" };
static const char* const SENS_LBL[3] = { "низька", "середня", "висока" };
static const uint8_t ACT_ORDER[8] = { MA_TOGGLE, MA_NEXT, MA_PREV, MA_VOLUP, MA_VOLDN, MA_SCREEN, MA_FAV1, MA_NONE };
static volatile uint8_t s_gKind = 0;

static void gestRow(Gfx& g, int16_t x, int16_t y, int16_t w, int16_t h, uint8_t r){
  drawCard(g, x, y, w, h, r == 0, r == 1);
  if(r) g.fill(x + 14, y, w - 14, 1, C_LINE);
  MicGesture gs = s_gKind ? (r ? MG_KNOCK3 : MG_KNOCK2) : (r ? MG_CLAP3 : MG_CLAP2);
  g.text(x + 14, y + h / 2 + 4, r ? "Тричі" : "Двічі", F_ROW, C_TXT);
  const int16_t bx = x + 96, bw = w - 96 - 10;
  g.box(bx, y + 7, bw, h - 14, 10, C_SURF2);
  icon(g, IC_BACK, bx + 14, y + h / 2, C_ACC, C_SURF2);
  icon(g, IC_CHEV, bx + bw - 13, y + h / 2, C_ACC, C_SURF2);
  g.text(bx + bw / 2, y + h / 2 + 4, YoMic::actionName(YoMic::actionFor(gs)), F_SMB, C_TXT, AL_C, bw - 50);
}
static void gestRow2(Gfx& g, int16_t x, int16_t y, int16_t w, int16_t h){ gestRow(g, x, y, w, h, 0); }
static void gestRow3(Gfx& g, int16_t x, int16_t y, int16_t w, int16_t h){ gestRow(g, x, y, w, h, 1); }
static const char* vHeard(){
  static char b[72];
  uint32_t now = millis();
  if(!extras.s.micOn) snprintf(b, sizeof(b), "мікрофон вимкнено — жести не слухаються");
  else if(mic.heard() && now - mic.heardMs() < 8000) snprintf(b, sizeof(b), tr("почуто: %s — %s"), tr(YoMic::gestureName(mic.heard())), tr(YoMic::actionName(YoMic::actionFor((MicGesture)mic.heard()))));
  else snprintf(b, sizeof(b), "плесніть чи постукайте двічі");
  return b;
}
static Item s_gestItems[] = {
  iSeg("", GTAB_LBL, 2, [](){ return (int32_t)s_gKind; }, [](int32_t v){ s_gKind = v; }),
  iSwitch("Увімкнено", IC_HAND, C_VIOLET, [](){ return (int32_t)(s_gKind ? extras.s.knockOn : extras.s.clapOn); },
          [](int32_t v){ ExtStore& e = extras.s; uint8_t& on = s_gKind ? e.knockOn : e.clapOn; on = v; if(on && !e.micOn){ e.micOn = 1; mic.apply(); } extras.changed(); }),
  iSeg("Чутливість", SENS_LBL, 3, [](){ uint8_t s = s_gKind ? extras.s.knockSens : extras.s.clapSens; return (int32_t)(s == 1 ? 0 : s == 2 ? 2 : 1); },
       [](int32_t v){ (s_gKind ? extras.s.knockSens : extras.s.clapSens) = v == 0 ? 1 : v == 2 ? 2 : 0; extras.changed(); }),
  iSection("ЩО РОБИТИ"),
  iCustom(46, gestRow2),
  iCustom(46, gestRow3),
  iNote(vHeard, 24),
};
class GestPage : public ListPage {
  public:
    GestPage() : ListPage("Хлопки й стук", s_gestItems, sizeof(s_gestItems) / sizeof(s_gestItems[0])) {}
    int16_t hit(int16_t x, int16_t y, Rect& r, uint8_t& radius) override {
      for(uint8_t k = 4; k <= 5; k++){
        const Item& it = s_gestItems[k];
        if(it.vis && y >= it.y && y < it.y + it.hh && x >= MX + 96){ r = Rect(MX + 96, it.y + 7, CWID - 106, it.hh - 14); radius = 10; return k; }
      }
      return ListPage::hit(x, y, r, radius);
    }
    void tap(int16_t id, int16_t x, int16_t y) override {
      if(id != 4 && id != 5){ ListPage::tap(id, x, y); return; }
      ExtStore& e = extras.s;
      uint8_t r = id - 4;
      MicGesture gs = s_gKind ? (r ? MG_KNOCK3 : MG_KNOCK2) : (r ? MG_CLAP3 : MG_CLAP2);
      uint8_t cur = YoMic::actionFor(gs), k = 0;
      for(uint8_t i = 0; i < 8; i++) if(ACT_ORDER[i] == cur) k = i;
      const int16_t mid = MX + 96 + (CWID - 106) / 2;
      k = x < mid ? (k + 7) % 8 : (k + 1) % 8;
      uint8_t& slot = s_gKind ? (r ? e.knock3 : e.knock2) : (r ? e.clap3 : e.clap2);
      slot = ACT_ORDER[k];
      extras.changed();
    }
    void tick(uint32_t now) override {
      ListPage::tick(now);
      static uint32_t t = 0; static uint32_t sig = 0;
      if(now - t >= 250){
        t = now;
        uint32_t s = s_gKind * 1000 + YoMic::actionFor(MG_CLAP2) + YoMic::actionFor(MG_CLAP3) * 10 + YoMic::actionFor(MG_KNOCK2) * 100 + YoMic::actionFor(MG_KNOCK3) * 7;
        if(s != sig){ sig = s; M.inval(Rect(0, s_gestItems[4].y, SW, 92)); }
      }
    }
};
static GestPage s_gest;
Page& pgGest = s_gest;

/*  =================== «Присутність» =================== */
static const char* const EARMIN_LBL[4] = { "5", "10", "15", "30" };
static const uint8_t     EARMIN_VAL[4] = { 5, 10, 15, 30 };
static const char* const POFF_LBL[4]   = { "ні", "5", "15", "30" };
static const uint8_t     POFF_VAL[4]   = { 0, 5, 15, 30 };
static void micNeeded(){ if(!extras.s.micOn){ extras.s.micOn = 1; mic.apply(); } }
static Item s_presItems[] = {
  iSection("ТАЙМЕР СНУ"),
  iSwitch("Таймер сну слухає", IC_MOON, C_VIOLET, [](){ return (int32_t)extras.s.sleepEar; },
          [](int32_t v){ extras.s.sleepEar = v; if(v) micNeeded(); extras.changed(); }),
  iSeg("У кімнаті тихо, хв", EARMIN_LBL, 4, [](){ uint8_t m = extras.s.sleepEarMin ? extras.s.sleepEarMin : 10; for(uint8_t i = 0; i < 4; i++) if(EARMIN_VAL[i] == m) return (int32_t)i; return (int32_t)1; },
       [](int32_t v){ extras.s.sleepEarMin = EARMIN_VAL[v]; extras.changed(); }),
  iSection("ЕКРАН"),
  iSwitch("Голос будить екран", IC_SUN, C_VIOLET, [](){ return (int32_t)extras.s.presWake; },
          [](int32_t v){ extras.s.presWake = v; if(v) micNeeded(); extras.changed(); }),
  iSeg("Гасити екран, коли тихо, хв", POFF_LBL, 4, [](){ for(uint8_t i = 0; i < 4; i++) if(POFF_VAL[i] == extras.s.presOff) return (int32_t)i; return (int32_t)0; },
       [](int32_t v){ extras.s.presOff = POFF_VAL[v]; if(extras.s.presOff) micNeeded(); extras.changed(); }),
  iNote([](){ return "мікрофон чує, чи є в кімнаті люди"; }, 24),
};
static ListPage s_pres("Присутність", s_presItems, sizeof(s_presItems) / sizeof(s_presItems[0]));
Page& pgPres = s_pres;

}  // namespace m2
