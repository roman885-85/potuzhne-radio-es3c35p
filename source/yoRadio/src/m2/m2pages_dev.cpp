/*  Нове меню: «Розробник», «Звуки й заставка», «Аудіовихід», схема ЦАП.  */
#include "../core/options.h"
#include "m2pages.h"
#include "m2lang.h"
#include "../core/config.h"
#include "../core/display.h"
#include "../extras/yoExtras.h"
#include "../extras/yoRecorder.h"
#include "../extras/yoLogos.h"
#include "../extras/yoSfx.h"

namespace m2 {

#if defined(PR_BOARD_ES3C35P)
/*  ES3C35P: VS1053B під'єднати нікуди (потрібно 7 сигналів, вільних на роз'ємах — 4).  */
static const char* const DAC_NAME[5] = { "ES8311", "PCM5100/5102A", "UDA1334A", "MAX98357A", "VS1053B" };
static const uint8_t DAC_N = 4;
#else
static const char* const DAC_NAME[5] = { "ES8311", "PCM5102A", "UDA1334A", "MAX98357A", "VS1053B" };
static const uint8_t DAC_N = 5;
#endif
static const char* const PWR_LBL[3] = { "вимк", "високий", "низький" };
static const char* const DAC_KIND[5] = { "вбудований кодек і підсилювач", "стерео ЦАП, лінійний вихід",
                                         "стерео ЦАП, навушники й лінія", "моно підсилювач 3 Вт на динамік",
                                         "окремий декодер, інша прошивка" };
static volatile int8_t s_dacSel = 0;
static volatile bool s_dacAlt = false;

/*  =================== «Розробник» =================== */
static const char* vDac(){ return DAC_NAME[extras.s.dac < 5 ? extras.s.dac : 0]; }
static const char* vSnd(){ return extras.s.splashOff ? (extras.s.sfxOn ? "звуки" : "вимк") : (extras.s.sfxOn ? "заставка, звуки" : "заставка"); }
static Item s_devItems[] = {
  iSection("ЩО ПОКАЗУВАТИ"),
  iSwitch("Батарея на екрані", IC_BATTERY, C_GREEN, [](){ return (int32_t)!extras.s.noBat; }, [](int32_t v){ extras.s.noBat = !v; extras.changed(); }),
  iSwitch("IP-адреса на екрані", IC_GLOBE, C_BLUE, [](){ return (int32_t)!extras.s.noIp; }, [](int32_t v){ extras.s.noIp = !v; extras.changed(); }),
  iSection("ОБЛАДНАННЯ"),
  iSwitch("Картка пам'яті", IC_CARD, C_ORANGE, [](){ return (int32_t)!extras.s.noSd; },
          [](int32_t v){ extras.s.noSd = !v; extras.changed(); if(extras.s.noSd){ recorder.stop(); if(config.getMode() == PM_SDCARD) config.changeMode(PM_WEB); } }),
#if EXT_DAC
  iNav("Аудіовихід", IC_SPEAKER, C_TEAL, vDac, [](){ M.push(&pgDac); }),
  iSeg("Ключ живлення модуля (" DAC_PWR_NAME ")", PWR_LBL, 3, [](){ return (int32_t)extras.s.dacPwr; },
       [](int32_t v){ extras.s.dacPwr = v; extras.changed(); extras.applyDac(); }),
  iNote([](){ return extras.s.dacPwr == 1
      ? "ДОСЛІД: " DAC_PWR_NAME " вмикає модуль ВИСОКИМ рівнем.\nСхема: P-канальний у розрив 5 В, затвор через 10 кОм\nна 5 В, NPN із затвора на землю, база через 10 кОм\nна " DAC_PWR_NAME ". Модуль гасне разом із радіо."
      : extras.s.dacPwr == 2
      ? "ДОСЛІД: " DAC_PWR_NAME " вмикає модуль НИЗЬКИМ рівнем.\nСхема простіша: один P-канальний, витік на 3,3 В,\nзатвор прямо на " DAC_PWR_NAME ". Годиться модулям, яким\nвистачає 3,3 В. Модуль гасне разом із радіо."
      : "ДОСЛІД: вимкнене радіо світить модулем — шина\n3,3 В мусить лишатись під напругою, на ній годинник\nі сенсор пробудження. " DAC_PWR_NAME " може гасити модуль через\nключ. Рівень виберіть за своїм транзистором."; }, 66),
#endif
  iNav("Заставка й звуки", IC_NOTE, C_PINK, vSnd, [](){ M.push(&pgDevSnd); }),
  iSection("ЛОГОТИПИ СТАНЦІЙ"),
  iButton("Завантажити всі логотипи", IC_REFRESH, [](){
    logos.fetchAll();
    char m[48]; snprintf(m, sizeof(m), tr("шукаю для %u станцій"), (unsigned)logos.batchTotal());
    M.toast(m);
  }),
  iNote([](){ static char b[64];
    if(logos.batchOn()) snprintf(b, sizeof(b), tr("іде: %u з %u, знайдено %u"), (unsigned)logos.batchDone(), (unsigned)logos.batchTotal(), (unsigned)logos.batchFound());
    else snprintf(b, sizeof(b), "%s", "для всіх станцій списку, у тлі, по одній");
    return (const char*)b; }, 22),
  iButton("Шукати логотипи знову", IC_REFRESH, [](){
    uint16_t n = logos.forget();
    char m[48]; snprintf(m, sizeof(m), tr("шукатиму знову: %u"), n);
    M.toast(m);
    display.forceLogo();
  }),
  iNote([](){ return "для станцій, де минулого разу не знайшлося"; }, 22),
  iSection("СЕНСОР"),
  iButton("Калібрування сенсора", IC_HAND, [](){ M.calibStart(); }),
  iNote([](){ return "якщо дотик б'є мимо: торкніться чотирьох позначок по кутах"; }, 22),
  iButton("Скинути калібрування", IC_RESTART, [](){ M.calibReset(); }),
};
static ListPage s_dev("Розробник", s_devItems, sizeof(s_devItems) / sizeof(s_devItems[0]));
Page& pgDev = s_dev;

/*  =================== «Заставка й звуки» =================== */
/*  Повзунок гучності події: під час руху — пробний звук (не частіше ніж раз на 0,7 с).  */
static void sfxHear(SfxEvent e){
  static uint32_t t = 0;
  if(millis() - t < 700) return;
  t = millis();
  sfx.test(e);
}
/*  Кнопка ▶ «прослухати»: грає звук події зараз, навіть коли звуки подій вимкнено.  */
static void sfxListen(SfxEvent e){
  const ExtStore& s = extras.s;
  /*  Батарея рахується лише за своїм повзунком — так само, як і звучить.  */
  uint32_t v = e == SFX_START ? s.splashVol
             : e == SFX_LOWBAT ? s.sfxEvVol[e]
             : (uint32_t)s.sfxVol * s.sfxEvVol[e] / 100;
  if(!v){ M.toast(e == SFX_START || (!s.sfxVol && e != SFX_LOWBAT) ? "гучність 0 — звуку не буде" : "гучність цього звуку 0"); return; }
  sfx.test(e);
}
#define SFX_EV_SLIDER(label, ev) iSliderPlay(label, 0, 100, [](){ return (int32_t)extras.s.sfxEvVol[ev]; }, \
  [](int32_t v){ extras.s.sfxEvVol[ev] = (uint8_t)v; extras.changed(); sfxHear(ev); }, "%", [](){ sfxListen(ev); })
static Item s_dsItems[] = {
  iSection("ЗАСТАВКА"),
  iSwitch("Анімована заставка", IC_SPLASH, C_PINK, [](){ return (int32_t)!extras.s.splashOff; }, [](int32_t v){ extras.s.splashOff = !v; extras.changed(); }),
  iSliderPlay("Привітання (звук заставки)", 0, 100, [](){ return (int32_t)extras.s.splashVol; }, [](int32_t v){ extras.s.splashVol = v; extras.changed(); sfxHear(SFX_START); }, "%",
              [](){ sfxListen(SFX_START); }),
  iButton("Показати заставку", IC_PLAY, [](){ afterClose = 2; M.close(); }),
  iSection("ЗВУКИ ПОДІЙ"),
  iSwitch("Звуки подій", IC_BELL, C_PINK, [](){ return (int32_t)extras.s.sfxOn; }, [](int32_t v){ extras.s.sfxOn = v; extras.changed(); }),
  iSlider("Загальна гучність", 0, 100, [](){ return (int32_t)extras.s.sfxVol; }, [](int32_t v){ extras.s.sfxVol = v; extras.changed(); sfxHear(SFX_GESTURE); }, "%"),
  iSection("ГУЧНІСТЬ КОЖНОГО ЗВУКУ"),
  SFX_EV_SLIDER("Дотик до екрана", SFX_CLICK),
  SFX_EV_SLIDER("Жест прийнято", SFX_GESTURE),
  SFX_EV_SLIDER("Мережа з'явилась", SFX_CONNECT),
  SFX_EV_SLIDER("Мережа зникла", SFX_ERROR),
  SFX_EV_SLIDER("Таймер сну", SFX_TIMER),
  SFX_EV_SLIDER("Будильник", SFX_ALARM),
  SFX_EV_SLIDER("Батарея сідає", SFX_LOWBAT),
  iNote([](){ return "кнопка з трикутником — прослухати звук;\nсвій звук (MP3 чи WAV) і які події озвучувати —\nна сторінці радіо: Розробник › Звуки подій"; }, 50),
};
static ListPage s_devSnd("Заставка й звуки", s_dsItems, sizeof(s_dsItems) / sizeof(s_dsItems[0]));
Page& pgDevSnd = s_devSnd;

/*  =================== «Аудіовихід» =================== */
class DacPage : public Page {
  public:
    const char* title() override { return "Аудіовихід"; }
    int16_t height() override { return 4 + DAC_N * 52 + 12; }
    void draw(Gfx& g) override {
      for(uint8_t i = 0; i < DAC_N; i++){
        int16_t y = 4 + i * 52;
        if(!g.visible(MX, y, CWID, 52)) continue;
        bool cur = extras.s.dac == i;
        drawCard(g, MX, y, CWID, 52, i == 0, i == DAC_N - 1);
        if(i) g.fill(MX + 50, y, CWID - 50, 1, C_LINE);
        drawBadge(g, MX + 12, y + 12, i == 4 ? IC_CHIP : IC_SPEAKER, cur ? C_TEAL : C_SURF2);
        g.text(MX + 50, y + 23, DAC_NAME[i], F_ROWB, cur ? C_TEAL : C_TXT);
        g.text(MX + 50, y + 41, cur ? "зараз звук іде сюди" : DAC_KIND[i], F_SM, cur ? C_TEAL : C_TXT2, AL_L, CWID - 80);
        icon(g, IC_CHEV, MX + CWID - 16, y + 26, C_TXT2, C_SURF);
      }
    }
    void tick(uint32_t now) override { static uint32_t t = 0; static uint8_t d = 255; if(now - t > 300){ t = now; if(d != extras.s.dac){ d = extras.s.dac; M.invalAll(); } } }
    int16_t hit(int16_t x, int16_t y, Rect& r, uint8_t& radius) override {
      (void)x;
      if(y < 4 || y >= 4 + DAC_N * 52) return -1;
      int16_t i = (y - 4) / 52;
      r = Rect(MX, 4 + i * 52, CWID, 52); radius = (i == 0 || i == DAC_N - 1) ? R_CARD : 0;
      return i;
    }
    void tap(int16_t id, int16_t x, int16_t y) override { (void)x; (void)y; if(id >= 0 && id < DAC_N){ s_dacSel = id; s_dacAlt = false; M.push(&pgDacInfo); } }
};
static DacPage s_dac;
Page& pgDac = s_dac;

/*  =================== схема підключення =================== */
class DacInfoPage : public Page {
  public:
    const char* title() override { return DAC_NAME[s_dacSel]; }
    bool scrollable() override { return false; }
    void enter() override { _sig = 0xFF; }
    void draw(Gfx& g) override;
    void tick(uint32_t now) override { (void)now; uint8_t s = extras.s.dac * 2 + s_dacAlt; if(s != _sig){ _sig = s; M.invalAll(); } }
    int16_t hit(int16_t x, int16_t y, Rect& r, uint8_t& radius) override {
      (void)x;
      if(y < 160 || y >= 196) return -1;
      if(s_dacSel != 4 && extras.s.dac == s_dacSel) return -1;
      r = Rect(MX, 160, CWID, 36); radius = 12; return 1;
    }
    void tap(int16_t id, int16_t x, int16_t y) override {
      (void)x; (void)y;
      if(id != 1) return;
      if(s_dacSel == 4){ s_dacAlt = !s_dacAlt; return; }
      extras.s.dac = s_dacSel; extras.changed(); extras.applyDac();
      M.toast("звук іде на цей вихід");
    }
  private:
    uint8_t _sig = 0xFF;
};

void DacInfoPage::draw(Gfx& g){
  struct Pin { const char* esp; const char* mod; };
  char t[64];
  if(s_dacSel == 4 && s_dacAlt){
    struct Hdr { const char* name; const char* pin[4]; uint8_t used; const char* to[4]; };
    static const Hdr H[3] = {
      { "роз'єм розширення (Expand)", { "IO2", "IO3", "IO14", "IO21" }, 0x0F, { "XCS", "MISO", "SCK", "MOSI" } },
      { "роз'єм UART",                { "TXD0", "RXD0", "GND", "5V" },   0x0F, { "XDCS", "DREQ", "GND", "5V" } },
      { "роз'єм I2C",                 { "3.3V", "GND", "SCL", "SDA" },   0x01, { "XRST", "", "", "" } },
    };
    for(uint8_t h = 0; h < 3; h++){
      int16_t y = 2 + h * 52;
      g.text(MX + 2, y + 11, H[h].name, F_SM, C_TXT2);
      for(uint8_t k = 0; k < 4; k++){
        int16_t x = MX + k * 76;
        bool on = H[h].used & (1 << k);
        g.box(x, y + 15, 72, 18, 6, on ? C_ACC : C_SURF2);
        g.text(x + 36, y + 28, H[h].pin[k], F_SMB, on ? C_ACCTXT : C_TXT2, AL_C);
        if(on && H[h].to[k][0]){ snprintf(t, sizeof(t), "→ %s", H[h].to[k]); g.text(x + 36, y + 47, t, F_SM, C_TXT, AL_C); }
      }
    }
    g.box(MX, 160, CWID, 36, 12, C_SURF2);
    g.text(SW / 2, 183, "Назад до схеми", F_ROWB, C_TXT, AL_C);
    return;
  }
#if defined(PR_BOARD_ES3C35P)
  /*  ES3C35P: Expand (P3) — IO45, IO46; UART (P2) — +5 В, GND, TXD0 (IO43)  */
  static const Pin P1[] = { {"5V UART","VIN"}, {"GND UART","GND"}, {"IO45","BCK"}, {"IO46","LCK"}, {"TXD0","DIN"}, {"GND Expand","SCK"}, {"3V3 Expand","XSMT"} };
  static const Pin P2[] = { {"5V UART","VIN"}, {"GND UART","GND"}, {"IO45","BCLK"}, {"IO46","WSEL"}, {"TXD0","DIN"} };
  static const Pin P3[] = { {"5V UART","VIN"}, {"GND UART","GND"}, {"IO45","BCLK"}, {"IO46","LRC"}, {"TXD0","DIN"} };
#else
  static const Pin P1[] = { {"5V UART","VIN"}, {"GND UART","GND"}, {"IO14","BCK"}, {"IO21","LCK"}, {"IO2","DIN"}, {"GND I2C","SCK"}, {"3V3 I2C","XSMT"} };
  static const Pin P2[] = { {"5V UART","VIN"}, {"GND UART","GND"}, {"IO14","BCLK"}, {"IO21","WSEL"}, {"IO2","DIN"} };
  static const Pin P3[] = { {"5V UART","VIN"}, {"GND UART","GND"}, {"IO14","BCLK"}, {"IO21","LRC"}, {"IO2","DIN"} };
#endif
  static const Pin P4[] = { {"5V UART","5V"}, {"GND UART","GND"}, {"IO14","SCK"}, {"IO21","MOSI"}, {"IO3","MISO"},
                            {"IO2","XCS"}, {"TXD0","XDCS"}, {"RXD0","DREQ"}, {"3V3 I2C","XRST"} };
  static const char* D[5][2] = {
    { "Кодек ES8311 + підсилювач SC8002B на динамік.", "ES8388 на цій платі немає." },
    { "Стерео ЦАП, лінійний вихід 3.5 мм.", "SCK на GND, XSMT на 3V3." },
    { "Стерео ЦАП: навушники й лінійний вихід.", "Живлення 3–5 В." },
    { "Моно підсилювач 3 Вт на динамік 4–8 Ом.", "GAIN і SD не під'єднувати." },
    { "Без пайки: роз'єми на звороті плати.", "Потрібна окрема прошивка." },
  };
  const Pin* pins = nullptr; uint8_t np = 0;
  switch(s_dacSel){ case 1: pins = P1; np = 7; break; case 2: pins = P2; np = 5; break; case 3: pins = P3; np = 5; break; case 4: pins = P4; np = 9; break; default: break; }
  /*  Досліду живлення з IO3 — показуємо саме те, що треба з'єднати зараз:
      провід живлення йде не на 5V роз'єму UART, а на IO3.  */
  static Pin alt[9];
  if(np && extras.s.dacPwr && s_dacSel >= 1 && s_dacSel <= 3){
    memcpy(alt, pins, np * sizeof(Pin));
    alt[0].esp = "5V через ключ";
    pins = alt;
  }
  int16_t ty;
  if(np){
    const int16_t rh = np > 7 ? 13 : (np > 5 ? 16 : 18), y0 = 16;
    g.text(MX + 2, 11, "плата (роз'єм)", F_SM, C_TXT2);
    g.text(MX + CWID - 2, 11, DAC_NAME[s_dacSel], F_SM, C_TXT2, AL_R);
    for(uint8_t k = 0; k < np; k++){
      int16_t y = y0 + k * rh; float cy = y + rh / 2.0f;
      bool pwr = !strncmp(pins[k].esp, "5V", 2) || !strncmp(pins[k].esp, "3V3", 3);
      bool gnd = !strncmp(pins[k].esp, "GND", 3);
      uint16_t wc = pwr ? C_RED : (gnd ? C_GREY : C_ACC);
      g.box(MX, y + 1, 76, rh - 2, 4, C_SURF2);
      g.box(MX + CWID - 62, y + 1, 62, rh - 2, 4, C_SURF2);
      g.line(MX + 76, cy, MX + CWID - 62, cy, 2, wc);
      g.circle(MX + 76, cy, 2.8f, wc); g.circle(MX + CWID - 62, cy, 2.8f, wc);
      g.text(MX + 5, (int16_t)cy + 4, pins[k].esp, F_SM, C_TXT);
      g.text(MX + CWID - 57, (int16_t)cy + 4, pins[k].mod, F_SM, C_TXT);
    }
    ty = y0 + np * rh + 14;
  }else{
    const char* blk[4] = { "ESP32-S3", "ES8311", "SC8002B", "динамік" };
    for(uint8_t k = 0; k < 4; k++){
      int16_t x = MX + k * 77;
      g.box(x, 30, 69, 44, 10, C_SURF);
      g.text(x + 34, 57, blk[k], F_SMB, C_TXT, AL_C, 64);
      if(k < 3) g.line(x + 69, 52, x + 77, 52, 2, C_ACC);
    }
    ty = 104;
  }
  g.text(MX + 2, ty, D[s_dacSel][0], F_SM, C_TXT2, AL_L, CWID);
  g.text(MX + 2, ty + 14, D[s_dacSel][1], F_SM, C_TXT2, AL_L, CWID);
  if(extras.s.dacPwr && s_dacSel >= 1 && s_dacSel <= 3)
    g.text(MX + 2, ty + 28, "Дослід: " DAC_PWR_NAME " керує ключем 5 В — модуль гасне з радіо.", F_SM, C_ORANGE, AL_L, CWID);
  if(s_dacSel == 4){
    g.box(MX, 160, CWID, 36, 12, C_SURF2);
    g.text(SW / 2, 183, "Де ці роз'єми на платі", F_ROWB, C_TXT, AL_C);
  }else{
    bool cur = extras.s.dac == s_dacSel;
    g.box(MX, 160, CWID, 36, 12, cur ? C_SURF : C_ACC);
    g.text(SW / 2, 183, cur ? "Зараз звук іде сюди" : "Увімкнути цей вихід", F_ROWB, cur ? C_TEAL : C_ACCTXT, AL_C);
  }
}

static DacInfoPage s_dacInfo;
Page& pgDacInfo = s_dacInfo;

}  // namespace m2
