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
static const char* const MUTE_LBL[3] = { "вимк", "тихо — низьким", "тихо — високим" };
/*  Підпис під MUTE зовнішнього підсилювача: що й куди під'єднати і що на виводі зараз (живе).  */
static const char* vMuteNote(){
  static char b[220];
  const uint8_t m = extras.s.extMute;
  if(!m) return "Вхід MUTE підсилювача (чи XSMT ЦАП) — на " EXT_MUTE_NAME ":\nрадіо глушитиме його, коли мовчить.";
  snprintf(b, sizeof(b), "%s\n%s\n%s",
           tr(m == 1 ? "Тихо — низьким: XSMT у PCM5102A, MUTE у PAM8403." : "Тихо — високим: MUTE у TPA3116."),
           tr(m == 1 ? "Резистор 10 кОм з " EXT_MUTE_NAME " на GND — тихо й на старті." : "Резистор 10 кОм з " EXT_MUTE_NAME " на 3,3 В — тихо й на старті."),
           extras.s.dac == 0 ? tr("зараз: тихо — звук іде на вбудований динамік") : tr(extras.extAmpOn() ? "зараз: звук" : "зараз: тихо"));
  return b;
}
static const char* const DAC_KIND[5] = { "вбудований кодек і підсилювач", "стерео ЦАП, лінійний вихід",
                                         "стерео ЦАП, навушники й лінія", "моно підсилювач 3 Вт на динамік",
                                         "окремий декодер, інша прошивка" };
static volatile int8_t s_dacSel = 0;
static volatile bool s_dacAlt = false;

/*  =================== «Розробник» =================== */
/*  Сітка розділів на один екран (власник 03.11.2026: «розбий довгі сторінки налаштувань на підсторінки»).  */
static const char* vDac(){ return DAC_NAME[extras.s.dac < 5 ? extras.s.dac : 0]; }
static const char* vSnd(){ return extras.s.splashOff ? (extras.s.sfxOn ? "звуки" : "вимк") : (extras.s.sfxOn ? "заставка, звуки" : "заставка"); }
static const char* vShow(){ const bool b = !extras.s.noBat, i = !extras.s.noIp; return b && i ? "батарея, IP" : b ? "батарея" : i ? "IP-адреса" : "нічого"; }
static const char* vDbg(){ return extras.s.noNetCon ? "консоль вимк" : "консоль :2323"; }
static const Cell s_devCells[] = {
  { "Що показувати",    IC_EYE,     C_GREEN,  vShow,   [](){ M.push(&pgDevShow); } },
  { "Обладнання",       IC_CHIP,    C_TEAL,   vDac,    [](){ M.push(&pgDevHw); } },
  { "Заставка й звуки", IC_NOTE,    C_PINK,   vSnd,    [](){ M.push(&pgDevSnd); } },
  { "Логотипи станцій", IC_REFRESH, C_BLUE,   nullptr, [](){ M.push(&pgLogos); } },
  { "Сенсор",           IC_HAND,    C_VIOLET, nullptr, [](){ M.push(&pgTouch); } },
  { "Налагодження",     IC_CODE,    C_ACC,    vDbg,    [](){ M.push(&pgDebug); } },
};
static GridPage s_dev("Розробник", s_devCells, sizeof(s_devCells) / sizeof(s_devCells[0]));
Page& pgDev = s_dev;

static Item s_devShowItems[] = {
  iSwitch("Батарея на екрані", IC_BATTERY, C_GREEN, [](){ return (int32_t)!extras.s.noBat; }, [](int32_t v){ extras.s.noBat = !v; extras.changed(); }),
  iSwitch("IP-адреса на екрані", IC_GLOBE, C_BLUE, [](){ return (int32_t)!extras.s.noIp; }, [](int32_t v){ extras.s.noIp = !v; extras.changed(); }),
};
static ListPage s_devShow("Що показувати", s_devShowItems, sizeof(s_devShowItems) / sizeof(s_devShowItems[0]));
Page& pgDevShow = s_devShow;

static const char* vMuteSum(){ return MUTE_LBL[extras.s.extMute < 3 ? extras.s.extMute : 0]; }
static Item s_devHwItems[] = {
  iSwitch("Картка пам'яті", IC_CARD, C_ORANGE, [](){ return (int32_t)!extras.s.noSd; },
          [](int32_t v){ extras.s.noSd = !v; extras.changed(); if(extras.s.noSd){ recorder.stop(); if(config.getMode() == PM_SDCARD) config.changeMode(PM_WEB); } }),
#if EXT_DAC
  iNav("Аудіовихід", IC_SPEAKER, C_TEAL, vDac, [](){ M.push(&pgDac); }),
  iNav("MUTE підсилювача", IC_SPEAKER, C_RED, vMuteSum, [](){ M.push(&pgMute); }),
#endif
};
static ListPage s_devHw("Обладнання", s_devHwItems, sizeof(s_devHwItems) / sizeof(s_devHwItems[0]));
Page& pgDevHw = s_devHw;

static Item s_muteItems[] = {
#if EXT_DAC
  iSeg("MUTE підсилювача (" EXT_MUTE_NAME ")", MUTE_LBL, 3, [](){ return (int32_t)extras.s.extMute; },
       [](int32_t v){ extras.s.extMute = v; extras.changed(); extras.extMuteApply(); }),
  iNote(vMuteNote, 50),
#else
  iNote([](){ return "на цій платі зовнішнього ЦАП немає"; }, 22),
#endif
};
static ListPage s_mute("MUTE підсилювача", s_muteItems, sizeof(s_muteItems) / sizeof(s_muteItems[0]));
Page& pgMute = s_mute;

static Item s_logoItems[] = {
  iButton("Завантажити всі логотипи", IC_REFRESH, [](){
    logos.fetchAllForce();
    char m[48]; snprintf(m, sizeof(m), tr("шукаю для %u станцій"), (unsigned)logos.batchTotal());
    M.toast(m);
  }),
  iNote([](){ static char b[128];
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
};
static ListPage s_logos("Логотипи станцій", s_logoItems, sizeof(s_logoItems) / sizeof(s_logoItems[0]));
Page& pgLogos = s_logos;

static Item s_touchItems[] = {
  iButton("Калібрування сенсора", IC_HAND, [](){ M.calibStart(); }),
  iNote([](){ return "якщо дотик б'є мимо:\nторкніться чотирьох позначок по кутах"; }, 36),
  iButton("Скинути калібрування", IC_RESTART, [](){ M.calibReset(); }),
};
static ListPage s_touch("Сенсор", s_touchItems, sizeof(s_touchItems) / sizeof(s_touchItems[0]));
Page& pgTouch = s_touch;

/*  «Інфо про потік» (журнал декодера: формат, частота, помилки) — сюди з «Параметрів»: це для налагодження.  */
static Item s_dbgItems[] = {
  iSwitch("Консоль по Wi-Fi", IC_CODE, C_TEAL, [](){ return (int32_t)!extras.s.noNetCon; }, [](int32_t v){ extras.s.noNetCon = !v; extras.changed(); }),
  iNote([](){ static char b[64]; char ip[16];
    if(!extras.s.noNetCon && yoNetConPeer(ip, sizeof(ip))) snprintf(b, sizeof(b), tr("під'єднано: %s"), ip);
    else snprintf(b, sizeof(b), "%s", tr("порт 2323, лише з вашої мережі"));
    return (const char*)b; }, 22),
  iSwitch("Інфо про потік", IC_INFO, C_TEAL, [](){ return (int32_t)config.store.audioinfo; },
          [](int32_t v){ config.saveValue(&config.store.audioinfo, static_cast<bool>(v)); }),
  iNote([](){ return "журнал декодера: формат, частота, помилки"; }, 22),
};
static ListPage s_debug("Налагодження", s_dbgItems, sizeof(s_dbgItems) / sizeof(s_dbgItems[0]));
Page& pgDebug = s_debug;

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
static const char* vSplash(){ static char b[32]; if(extras.s.splashOff) return "вимк"; snprintf(b, sizeof(b), tr("увімк · %u %%"), (unsigned)extras.s.splashVol); return b; }
static const char* vSfxSum(){ static char b[32]; if(!extras.s.sfxOn) return "вимк"; snprintf(b, sizeof(b), tr("увімк · %u %%"), (unsigned)extras.s.sfxVol); return b; }
static Item s_dsItems[] = {
  iNav("Заставка", IC_SPLASH, C_PINK, vSplash, [](){ M.push(&pgSplash); }),
  iNav("Звуки подій", IC_BELL, C_PINK, vSfxSum, [](){ M.push(&pgSfx); }),
  iNav("Гучність кожного звуку", IC_NOTE, C_PINK, nullptr, [](){ M.push(&pgSfxVol); }),
  iNote([](){ return "кнопка з трикутником — прослухати звук;\nсвій звук (MP3 чи WAV) і які події озвучувати —\nна сторінці радіо: Розробник › Звуки подій"; }, 50),
};
static ListPage s_devSnd("Заставка й звуки", s_dsItems, sizeof(s_dsItems) / sizeof(s_dsItems[0]));
Page& pgDevSnd = s_devSnd;

static Item s_splashItems[] = {
  iSwitch("Анімована заставка", IC_SPLASH, C_PINK, [](){ return (int32_t)!extras.s.splashOff; }, [](int32_t v){ extras.s.splashOff = !v; extras.changed(); }),
  iSliderPlay("Привітання (звук заставки)", 0, 100, [](){ return (int32_t)extras.s.splashVol; }, [](int32_t v){ extras.s.splashVol = v; extras.changed(); sfxHear(SFX_START); }, "%",
              [](){ sfxListen(SFX_START); }),
  iButton("Показати заставку", IC_PLAY, [](){ afterClose = 2; M.close(); }),
};
static ListPage s_splash("Заставка", s_splashItems, sizeof(s_splashItems) / sizeof(s_splashItems[0]));
Page& pgSplash = s_splash;

static Item s_sfxItems[] = {
  iSwitch("Звуки подій", IC_BELL, C_PINK, [](){ return (int32_t)extras.s.sfxOn; }, [](int32_t v){ extras.s.sfxOn = v; extras.changed(); }),
  iSlider("Загальна гучність", 0, 100, [](){ return (int32_t)extras.s.sfxVol; }, [](int32_t v){ extras.s.sfxVol = v; extras.changed(); sfxHear(SFX_GESTURE); }, "%"),
  iNote([](){ return "гучність кожного звуку — від загальної"; }, 22),
};
static ListPage s_sfx("Звуки подій", s_sfxItems, sizeof(s_sfxItems) / sizeof(s_sfxItems[0]));
Page& pgSfx = s_sfx;

/*  Гучність кожного звуку: сім рядків по 26 — назва, повзунок, відсотки, ▶. Звичайні повзунки (58 кожен)
    давали 7 × 58 = 406 — кілька екранів гортання.  */
struct SfxRow { const char* l; SfxEvent e; };
static const SfxRow SFX_ROWS[7] = { { "Дотик до екрана", SFX_CLICK }, { "Жест прийнято", SFX_GESTURE }, { "Мережа з'явилась", SFX_CONNECT },
                                    { "Мережа зникла", SFX_ERROR }, { "Таймер сну", SFX_TIMER }, { "Будильник", SFX_ALARM }, { "Батарея сідає", SFX_LOWBAT } };
class SfxVolPage : public Page {
  public:
    const char* title() override { return "Гучність кожного звуку"; }
    bool scrollable() override { return false; }
    void enter() override { _drag = -1; }
    void draw(Gfx& g) override {
      g.box(MX, Y0, CWID, RH * 7, R_CARD, C_SURF);
      char b[8];
      for(uint8_t i = 0; i < 7; i++){
        const int16_t y = Y0 + i * RH, cy = y + RH / 2;
        if(!g.visible(MX, y, CWID, RH)) continue;
        if(i) g.fill(MX + 12, y, CWID - 24, 1, C_LINE);
        const int16_t v = (_drag == i) ? _dragV : extras.s.sfxEvVol[SFX_ROWS[i].e];
        g.text(MX + 12, cy + 4, SFX_ROWS[i].l, F_SM, C_TXT, AL_L, TX - MX - 18);
        drawSlider(g, TX, cy, TW, v / 100.0f);
        snprintf(b, sizeof(b), "%d%%", (int)v);
        g.text(MX + CWID - 32, cy + 4, b, F_SM, _drag == i ? C_ACC : C_TXT2, AL_R);
        icon(g, IC_PLAY, MX + CWID - 15, cy, C_ACC, C_SURF);
      }
    }
    int16_t hit(int16_t x, int16_t y, Rect& r, uint8_t& radius) override {
      if(y < Y0 || y >= Y0 + RH * 7) return -1;
      const int16_t i = (y - Y0) / RH;
      if(x >= MX + CWID - 30){ r = Rect(MX + CWID - 30, Y0 + i * RH, 30, RH); radius = 0; return 100 + i; }
      r = Rect(MX, Y0 + i * RH, CWID - 30, RH); radius = 0;
      return i;
    }
    uint8_t grab(int16_t id) override { return (id >= 0 && id < 7) ? 1 : 0; }
    void drag(int16_t id, int16_t x, int16_t y, bool end) override {
      (void)y;
      if(id < 0 || id >= 7) return;
      const int16_t v = at(x);
      if(end){ _drag = -1; M.postValue(id, v, true); }
      else if(_drag != id || _dragV != v){ _drag = (int8_t)id; _dragV = v; M.postValue(id, v, false); }
      M.inval(Rect(0, Y0 + id * RH, SW, RH));
    }
    void value(int16_t id, int32_t v) override {
      if(id < 0 || id >= 7) return;
      extras.s.sfxEvVol[SFX_ROWS[id].e] = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
      extras.changed();
      sfxHear(SFX_ROWS[id].e);
    }
    void tap(int16_t id, int16_t x, int16_t y) override {
      (void)y;
      if(id >= 100 && id < 107){ sfxListen(SFX_ROWS[id - 100].e); return; }
      if(id >= 0 && id < 7 && x >= TX - 6){ value(id, at(x)); M.inval(Rect(0, Y0 + id * RH, SW, RH)); }
    }
  private:
    static constexpr int16_t RH = 26, Y0 = 4, TX = MX + 132, TW = CWID - 132 - 66;
    int8_t _drag = -1;
    int16_t _dragV = 0;
    static int16_t at(int16_t x){ int v = (int)lroundf((x - TX) * 100.0f / TW); return v < 0 ? 0 : v > 100 ? 100 : v; }
};
static SfxVolPage s_sfxVol;
Page& pgSfxVol = s_sfxVol;

/*  =================== «Аудіовихід» =================== */
/*  Рядок — 52, але щоб усі виходи стали на екран без прокрутки (на ES3C35P їх чотири: 4·52 + 16 = 224 > 200,
    останній обрізався — власник 02.11.2026), рядок стискається до 46.  */
static int16_t dacRowH(){ int16_t h = (CH - 16) / DAC_N; return h > 52 ? 52 : (h < 44 ? 44 : h); }
class DacPage : public Page {
  public:
    const char* title() override { return "Аудіовихід"; }
    int16_t height() override { return 4 + DAC_N * dacRowH() + 12; }
    void draw(Gfx& g) override {
      const int16_t rh = dacRowH(), k = (52 - rh) / 2;          /* на скільки підняти вміст рядка */
      for(uint8_t i = 0; i < DAC_N; i++){
        int16_t y = 4 + i * rh;
        if(!g.visible(MX, y, CWID, rh)) continue;
        bool cur = extras.s.dac == i;
        drawCard(g, MX, y, CWID, rh, i == 0, i == DAC_N - 1);
        if(i) g.fill(MX + 50, y, CWID - 50, 1, C_LINE);
        drawBadge(g, MX + 12, y + 12 - k, i == 4 ? IC_CHIP : IC_SPEAKER, cur ? C_TEAL : C_SURF2);
        g.text(MX + 50, y + 23 - k, DAC_NAME[i], F_ROWB, cur ? C_TEAL : C_TXT);
        g.text(MX + 50, y + 41 - k, cur ? "зараз звук іде сюди" : DAC_KIND[i], F_SM, cur ? C_TEAL : C_TXT2, AL_L, CWID - 80);
        icon(g, IC_CHEV, MX + CWID - 16, y + 26 - k, C_TXT2, C_SURF);
      }
    }
    void tick(uint32_t now) override { static uint32_t t = 0; static uint8_t d = 255; if(now - t > 300){ t = now; if(d != extras.s.dac){ d = extras.s.dac; M.invalAll(); } } }
    int16_t hit(int16_t x, int16_t y, Rect& r, uint8_t& radius) override {
      (void)x;
      const int16_t rh = dacRowH();
      if(y < 4 || y >= 4 + DAC_N * rh) return -1;
      int16_t i = (y - 4) / rh;
      r = Rect(MX, 4 + i * rh, CWID, rh); radius = (i == 0 || i == DAC_N - 1) ? R_CARD : 0;
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
  /*  MUTE «тихо — низьким» на PCM5102A — це його вхід XSMT: показуємо, що XSMT іде не на 3V3, а на
      вивід MUTE (з резистором 10 кОм на GND).  */
  static Pin alt[9];
  if(np && extras.s.extMute == 1 && s_dacSel == 1){
    memcpy(alt, pins, np * sizeof(Pin));
    for(uint8_t k = 0; k < np; k++) if(!strcmp(alt[k].mod, "XSMT")) alt[k].esp = EXT_MUTE_NAME;
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
  /*  Підказка про MUTE. З «тихо — низьким» на PCM5102A про нього вже каже другий рядок (XSMT іде на вивід MUTE,
      а не на 3V3 — підпис не має суперечити таблиці). У PCM5102A сім виводів, і третій рядок упирався в кнопку —
      підказки не було видно зовсім; тоді вона стає замість першого рядка (опис є й у списку виходів).  */
  static const char* const MUTE_LINE = "MUTE: " EXT_MUTE_NAME " — до XSMT ЦАП чи входу MUTE підсилювача.";
  const bool muteNote = extras.s.extMute && s_dacSel >= 1 && s_dacSel <= 3 && pins != alt;
  const bool line3 = ty + 28 <= 160 - 6;          /* третій рядок вміщається над кнопкою */
  const bool first = muteNote && !line3;
  g.text(MX + 2, ty, first ? MUTE_LINE : D[s_dacSel][0], F_SM, first ? C_ORANGE : C_TXT2, AL_L, CWID);
  g.text(MX + 2, ty + 14, (pins == alt) ? "SCK на GND, XSMT на " EXT_MUTE_NAME " (MUTE)." : D[s_dacSel][1], F_SM, C_TXT2, AL_L, CWID);
  if(muteNote && line3) g.text(MX + 2, ty + 28, MUTE_LINE, F_SM, C_ORANGE, AL_L, CWID);
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
