/*  Нове меню: «Wi-Fi», «Відомі мережі», знайома мережа, підключення,
    клавіатура. Уся робота з радіомодулем — функціями старого меню (WB).  */
#include "../core/options.h"
#include "m2pages.h"
#include "m2lang.h"
#include "m2bridge.h"
#include "../core/network.h"

namespace m2 {

static void connectNow(){
  WB::connect();
  if(M.ltop() == &pgConnect) pgConnect.enter();     /* «ще раз» зі сторінки підключення — та сама сторінка */
  else M.push(&pgConnect);
}
static char s_kbdTitle[64];
static void afterPass(bool ok){ if(ok) connectNow(); }

/*  =================== «Wi-Fi» =================== */
class WifiPage : public Page {
  public:
    const char* title() override { return "Wi-Fi"; }
    void enter() override { WB::load(); WB::pages(true, false); if(!WB::scanning()) WB::scanStart(); _sig = 0; }
    void leave() override { WB::pages(false, false); }
    bool canBack() override { return !WB::apLock(); }
    bool keepOpen() override { return true; }
    int16_t height() override { uint8_t n = WB::scanCount(); int16_t h = ROWS0 + (n ? n : 1) * RH + 12 + 3 * 44 + 12; return h < CH ? CH : h; }
    void draw(Gfx& g) override;
    void tick(uint32_t now) override;
    void loop(uint32_t now) override;
    int16_t hit(int16_t x, int16_t y, Rect& r, uint8_t& radius) override;
    void tap(int16_t id, int16_t x, int16_t y) override;
  private:
    static const int16_t ROWS0 = 4 + 54 + 30, RH = 42;
    uint32_t _sig = 0, _sigT = 0;
    int16_t actY(){ uint8_t n = WB::scanCount(); return ROWS0 + (n ? n : 1) * RH + 12; }
};

void WifiPage::draw(Gfx& g){
  char b[64];
  /*  поточна мережа  */
  if(g.visible(MX, 4, CWID, 54)){
    bool up = WB::staUp();
    g.box(MX, 4, CWID, 54, R_CARD, C_SURF);
    drawBadge(g, MX + 12, 17, IC_WIFI, up ? C_TEAL : C_GREY);
    g.text(MX + 50, 27, up ? WB::curSsid() : "не підключено", F_ROWB, C_TXT, AL_L, CWID - 64);
    if(up) snprintf(b, sizeof(b), tr("підключено · %s"), WB::ip());
    else snprintf(b, sizeof(b), WB::apLock() ? "виберіть мережу, щоб радіо почало грати" : "радіо зараз не в мережі");
    g.text(MX + 50, 45, b, F_SM, up ? C_TEAL : C_TXT2, AL_L, CWID - 64);
  }
  g.text(MX + 12, ROWS0 - 9, "ПОРУЧ", F_SMB, C_TXT2);
  if(WB::scanning()) g.text(MX + CWID - 12, ROWS0 - 9, "шукаю…", F_SM, C_ACC, AL_R);
  uint8_t n = WB::scanCount();
  if(!n){
    int16_t y = ROWS0;
    if(g.visible(MX, y, CWID, RH)){
      g.box(MX, y, CWID, RH, R_CARD, C_SURF);
      g.text(MX + 14, y + 26, WB::scanning() ? "шукаю мережі…" : "мереж не знайдено", F_ROW, C_TXT2);
    }
  }
  for(uint8_t i = 0; i < n; i++){
    int16_t y = ROWS0 + i * RH;
    if(!g.visible(MX, y, CWID, RH)) continue;
    drawCard(g, MX, y, CWID, RH, i == 0, i == n - 1);
    if(i) g.fill(MX + 14, y, CWID - 14, 1, C_LINE);
    const char* s = WB::scanSsid(i);
    bool cur = WB::staUp() && !strcmp(s, WB::curSsid());
    bool saved = WB::isSaved(s);
    int8_t rs = WB::scanRssi(i);
    uint8_t lv = rs > -55 ? 4 : rs > -65 ? 3 : rs > -75 ? 2 : rs > -85 ? 1 : 0;
    signalBars(g, MX + CWID - 32, y + 27, lv, C_TXT, C_SURF2);
    int16_t right = MX + CWID - 40;
    if(!WB::scanOpen(i)){ icon(g, IC_LOCK, right - 6, y + 21, C_TXT2, C_SURF); right -= 18; }
    if(cur){ icon(g, IC_CHECK, right - 8, y + 21, C_TEAL, C_SURF); right -= 20; }
    else if(saved){ g.circle(right - 5, y + 21, 3, C_ACC); right -= 14; }
    g.text(MX + 14, y + 26, s, F_ROW, cur ? C_TEAL : C_TXT, AL_L, right - MX - 20);
  }
  static const char* A[3] = { "Шукати ще раз", "Додати вручну", "Відомі мережі" };
  static const uint8_t AI[3] = { IC_REFRESH, IC_KEYS, IC_LIST };
  int16_t ay = actY();
  for(uint8_t i = 0; i < 3; i++){
    int16_t y = ay + i * 44;
    if(!g.visible(MX, y, CWID, 44)) continue;
    drawCard(g, MX, y, CWID, 44, i == 0, i == 2);
    if(i) g.fill(MX + 50, y, CWID - 50, 1, C_LINE);
    drawBadge(g, MX + 12, y + 8, AI[i], i == 0 ? C_BLUE : i == 1 ? C_GREY : C_TEAL);
    g.text(MX + 50, y + 26, A[i], F_ROW, (i == 0 && WB::scanning()) ? C_TXT3 : C_TXT);
    if(i == 2){ snprintf(b, sizeof(b), "%u", (unsigned)WB::savedCount()); g.text(MX + CWID - 28, y + 26, b, F_SM, C_TXT2, AL_R); }
    icon(g, IC_CHEV, MX + CWID - 16, y + 22, C_TXT2, C_SURF);
  }
}

void WifiPage::tick(uint32_t now){
  if(now - _sigT < 200) return;
  _sigT = now;
  uint32_t s = WB::scanCount() * 7 + (WB::scanning() ? 1 : 0) + (WB::staUp() ? 1000 : 0) + WB::savedCount() * 13;
  for(uint8_t i = 0; i < WB::scanCount(); i++){ const char* p = WB::scanSsid(i); while(*p) s = s * 31 + (uint8_t)*p++; s += (WB::scanRssi(i) + 100) / 10; }
  const char* c = WB::curSsid(); while(*c) s = s * 31 + (uint8_t)*c++;
  if(s != _sig){ _sig = s; M.invalAll(); }
}

void WifiPage::loop(uint32_t now){
  (void)now;
  /*  без мережі меню не мало виходу; радіо саме повернулось у збережену — відпускаємо на плеєр  */
  if(WB::apLock() && network.status == CONNECTED){ WB::apUnlock(); M.close(); }
}

int16_t WifiPage::hit(int16_t x, int16_t y, Rect& r, uint8_t& radius){
  (void)x;
  uint8_t n = WB::scanCount();
  if(y >= ROWS0 && y < ROWS0 + n * RH){
    int16_t i = (y - ROWS0) / RH;
    r = Rect(MX, ROWS0 + i * RH, CWID, RH); radius = (i == 0 || i == n - 1) ? R_CARD : 0;
    return i;
  }
  int16_t ay = actY();
  if(y >= ay && y < ay + 3 * 44){
    int16_t i = (y - ay) / 44;
    r = Rect(MX, ay + i * 44, CWID, 44); radius = (i == 0 || i == 2) ? R_CARD : 0;
    return 100 + i;
  }
  return -1;
}

void WifiPage::tap(int16_t id, int16_t x, int16_t y){
  (void)x; (void)y;
  if(id >= 0 && id < WB::scanCount()){
    uint8_t k = WB::pick(id);
    if(k == 1){ M.push(&pgNet); return; }
    if(k == 2){ WB::connect(); M.push(&pgConnect); return; }
    snprintf(s_kbdTitle, sizeof(s_kbdTitle), tr("Пароль: %s"), WB::ssidBuf());
    kbdOpen(WB::passBuf(), WB::passCap(), true, s_kbdTitle, afterPass);
    return;
  }
  if(id == 100){ if(!WB::scanning()) WB::scanStart(); return; }
  if(id == 101){
    WB::ssidBuf()[0] = 0; WB::passBuf()[0] = 0;
    kbdOpen(WB::ssidBuf(), WB::ssidCap(), false, "Назва мережі", [](bool ok){
      if(!ok || !WB::ssidBuf()[0]) return;
      snprintf(s_kbdTitle, sizeof(s_kbdTitle), tr("Пароль: %s"), WB::ssidBuf());
      kbdOpen(WB::passBuf(), WB::passCap(), true, s_kbdTitle, afterPass);
    });
    return;
  }
  if(id == 102){ WB::load(); M.push(&pgSaved); return; }
}

static WifiPage s_wifi;
Page& pgWifi = s_wifi;

/*  =================== «Відомі мережі» =================== */
class SavedPage : public Page {
  public:
    const char* title() override { return "Відомі мережі"; }
    void enter() override { WB::load(); _arm = -1; }
    bool keepOpen() override { return true; }
    void draw(Gfx& g) override;
    void tick(uint32_t now) override;
    int16_t hit(int16_t x, int16_t y, Rect& r, uint8_t& radius) override;
    void tap(int16_t id, int16_t x, int16_t y) override;
  private:
    volatile int8_t _arm = -1; uint32_t _armT = 0; uint32_t _sig = 0, _sigT = 0;
    static const int16_t Y0 = 4, RH = 48;
};

void SavedPage::draw(Gfx& g){
  uint8_t n = WB::savedCount();
  if(!n){
    g.box(MX, 20, CWID, 90, R_CARD, C_SURF);
    g.text(SW / 2, 56, "жодної мережі не збережено", F_ROW, C_TXT, AL_C);
    g.text(SW / 2, 80, "виберіть мережу зі списку — радіо запам'ятає її", F_SM, C_TXT2, AL_C, CWID - 20);
    return;
  }
  char b[8];
  for(uint8_t i = 0; i < n; i++){
    int16_t y = Y0 + i * RH;
    if(!g.visible(MX, y, CWID, RH)) continue;
    bool armed = _arm == (int8_t)i;
    drawCard(g, MX, y, CWID, RH, i == 0, i == n - 1, armed ? Gfx::blend(C_SURF, C_RED, 70) : C_SURF);
    if(i) g.fill(MX + 46, y, CWID - 46, 1, C_LINE);
    const char* s = WB::savedSsid(i);
    bool cur = WB::staUp() && !strcmp(s, WB::curSsid());
    g.circle(MX + 24, y + RH / 2, 12, i == 0 ? C_ACC : C_SURF2);
    snprintf(b, sizeof(b), "%u", (unsigned)(i + 1));
    g.text(MX + 24, y + RH / 2 + 4, b, F_SMB, i == 0 ? C_ACCTXT : C_TXT, AL_C);
    g.text(MX + 46, y + 22, armed ? "торкніться ще раз — забуду" : s, F_ROW, armed ? C_RED : C_TXT, AL_L, CWID - 46 - 90);
    g.text(MX + 46, y + 39, cur ? "радіо зараз тут" : (i == 0 ? "з неї радіо починає" : "запасна"), F_SM, cur ? C_TEAL : C_TXT2, AL_L, CWID - 136);
    if(i > 0){ g.circle(MX + CWID - 62, y + RH / 2, 15, C_SURF2); icon(g, IC_UP, MX + CWID - 62, y + RH / 2, C_ACC, C_SURF2); }
    g.circle(MX + CWID - 24, y + RH / 2, 15, armed ? C_RED : C_SURF2);
    icon(g, IC_TRASH, MX + CWID - 24, y + RH / 2, armed ? 0xFFFF : C_RED, armed ? C_RED : C_SURF2);
  }
  int16_t y = Y0 + n * RH + 16;
  g.text(MX + 12, y, "перша — з неї радіо починає; немає її в ефірі —", F_SM, C_TXT2, AL_L, CWID - 20);
  g.text(MX + 12, y + 14, "перебере решту по черзі", F_SM, C_TXT2, AL_L, CWID - 20);
}

void SavedPage::tick(uint32_t now){
  if(_arm >= 0 && now - _armT > 4000){ _arm = -1; M.invalAll(); }
  if(now - _sigT < 250) return;
  _sigT = now;
  uint32_t s = WB::savedCount() + (WB::staUp() ? 100 : 0);
  for(uint8_t i = 0; i < WB::savedCount(); i++){ const char* p = WB::savedSsid(i); while(*p) s = s * 31 + (uint8_t)*p++; }
  if(s != _sig){ _sig = s; M.invalAll(); }
}

int16_t SavedPage::hit(int16_t x, int16_t y, Rect& r, uint8_t& radius){
  uint8_t n = WB::savedCount();
  if(y < Y0 || y >= Y0 + n * RH) return -1;
  int16_t i = (y - Y0) / RH, cy = Y0 + i * RH + RH / 2;
  if(x >= MX + CWID - 42){ r = Rect(MX + CWID - 40, cy - 16, 32, 32); radius = 16; return 200 + i; }
  if(x >= MX + CWID - 80 && i > 0){ r = Rect(MX + CWID - 78, cy - 16, 32, 32); radius = 16; return 100 + i; }
  return -1;
}

void SavedPage::tap(int16_t id, int16_t x, int16_t y){
  (void)x; (void)y;
  if(id >= 200){
    uint8_t i = id - 200;
    if(_arm == (int8_t)i && millis() - _armT > 400){ _arm = -1; WB::savedForget(i); M.toast("мережу забуто"); }
    else { _arm = i; _armT = millis(); }
    M.invalAll();
  }else if(id >= 100){
    _arm = -1; WB::savedFirst(id - 100); M.toast("тепер вона перша");
    M.invalAll();
  }
}

static SavedPage s_saved;
Page& pgSaved = s_saved;

/*  =================== знайома мережа =================== */
static volatile int8_t s_forgetArm = 0; static uint32_t s_forgetT = 0;
static const char* vForget(){ return (s_forgetArm && millis() - s_forgetT < 4000) ? "Торкніться ще раз — забуду" : "Забути мережу"; }
static Item s_netItems[] = {
  iGap(10),
  iButton("Підключитись", IC_WIFI, connectNow),
  iNote([](){ return "радіо спробує просто зараз і скаже, чи вийшло"; }, 26),
  iButton("Змінити пароль", IC_KEYS, [](){
    snprintf(s_kbdTitle, sizeof(s_kbdTitle), tr("Пароль: %s"), WB::ssidBuf());
    kbdOpen(WB::passBuf(), WB::passCap(), true, s_kbdTitle, afterPass);
  }, C_TXT),
  iGap(12),
  iButton("Забути мережу", IC_TRASH, [](){
    if(s_forgetArm && millis() - s_forgetT > 400 && millis() - s_forgetT < 4000){
      s_forgetArm = 0; WB::forgetCurrent(); M.toast("мережу забуто"); M.pop();
    }else{ s_forgetArm = 1; s_forgetT = millis(); }
  }, C_RED),
};
class NetPage : public ListPage {
  public:
    NetPage() : ListPage("", s_netItems, sizeof(s_netItems) / sizeof(s_netItems[0])) {}
    const char* title() override { return WB::ssidBuf(); }
    void enter() override { s_forgetArm = 0; s_netItems[5].text = vForget; ListPage::enter(); }
    bool keepOpen() override { return true; }
};
static NetPage s_net;
Page& pgNet = s_net;

/*  =================== підключення =================== */
class ConnectPage : public Page {
  public:
    const char* title() override { return WB::ssidBuf(); }
    void enter() override { WB::pages(true, true); _okAt = 0; _shown = -1; }
    void leave() override { if(network.tryState() >= TRY_OK) network.tryClear(); WB::pages(true, false); }
    bool keepOpen() override { return true; }
    bool scrollable() override { return false; }
    bool canBack() override { return true; }
    void draw(Gfx& g) override;
    void tick(uint32_t now) override;
    void loop(uint32_t now) override;
    int16_t hit(int16_t x, int16_t y, Rect& r, uint8_t& radius) override;
    void tap(int16_t id, int16_t x, int16_t y) override;
  private:
    uint32_t _okAt = 0; int8_t _shown = -1;
    static bool failed(){ n_Try_e s = network.tryState(); return s == TRY_BADPASS || s == TRY_NOTFOUND || s == TRY_FAIL; }
};

void ConnectPage::draw(Gfx& g){
  n_Try_e st = network.tryState();
  const float cx = SW / 2, cy = 58;
  if(st == TRY_OK){
    g.circle(cx, cy, 32, C_TEAL);
    g.line(cx - 13, cy + 1, cx - 4, cy + 10, 5, C_BG); g.line(cx - 4, cy + 10, cx + 14, cy - 9, 5, C_BG);
  }else if(failed()){
    g.circle(cx, cy, 32, Gfx::blend(C_BG, C_RED, 200));
    g.line(cx - 11, cy - 11, cx + 11, cy + 11, 5, C_BG); g.line(cx - 11, cy + 11, cx + 11, cy - 11, 5, C_BG);
  }else{
    /*  крутиться, поки радіо пробує  */
    float a = fmodf(millis() * 0.36f, 360.0f);
    g.arc(cx, cy, 28, 5, C_SURF2);
    g.arc(cx, cy, 28, 5, C_ACC, a, a + 100);
    icon(g, IC_WIFI, cx, cy - 2, C_TXT2, C_BG);
  }
  const char* big = st == TRY_OK ? "Готово" : st == TRY_BADPASS ? "Невірний пароль" : st == TRY_NOTFOUND ? "Мережі не видно" : st == TRY_FAIL ? "Не вдалося" : "Підключаюсь…";
  const char* sub = st == TRY_OK ? WB::ip() : st == TRY_BADPASS ? "перевірте й введіть ще раз" : st == TRY_NOTFOUND ? "вона зникла з ефіру" : st == TRY_FAIL ? "мережа не відповідає" : "це займає кілька секунд";
  g.text(SW / 2, 122, big, F_MID, st == TRY_OK ? C_TEAL : (failed() ? C_TXT : C_TXT), AL_C, CWID);
  g.text(SW / 2, 142, sub, F_ROW, C_TXT2, AL_C, CWID);
  if(failed()){
    int16_t w = (CWID - 10) / 2;
    g.box(MX, 156, w, 38, R_BTN + 2, C_ACC);
    g.text(MX + w / 2, 180, "Ще раз", F_ROWB, C_ACCTXT, AL_C);
    g.box(MX + w + 10, 156, w, 38, R_BTN + 2, C_SURF2);
    g.text(MX + w + 10 + w / 2, 180, "До списку", F_ROWB, C_TXT, AL_C);
  }
}

void ConnectPage::tick(uint32_t now){
  (void)now;
  int8_t st = (int8_t)network.tryState();
  if(st != _shown){ _shown = st; M.invalAll(); }
  if(!failed() && st != TRY_OK) M.inval(Rect(SW / 2 - 40, 20, 80, 80));
}

void ConnectPage::loop(uint32_t now){
  if(network.tryState() == TRY_OK && !_okAt){ WB::saveCurrent(); _okAt = now; }
  /*  вийшло — трохи показуємо адресу й повертаємось на плеєр  */
  if(_okAt && now - _okAt > 1800){ _okAt = 0; WB::apUnlock(); network.tryClear(); M.close(); }
}

int16_t ConnectPage::hit(int16_t x, int16_t y, Rect& r, uint8_t& radius){
  if(!failed() || y < 156 || y >= 194) return -1;
  int16_t w = (CWID - 10) / 2;
  if(x < MX + w){ r = Rect(MX, 156, w, 38); radius = 12; return 0; }
  r = Rect(MX + w + 10, 156, w, 38); radius = 12; return 1;
}

void ConnectPage::tap(int16_t id, int16_t x, int16_t y){
  (void)x; (void)y;
  if(!failed()) return;
  network.tryClear();
  if(id == 0){
    snprintf(s_kbdTitle, sizeof(s_kbdTitle), tr("Пароль: %s"), WB::ssidBuf());
    kbdOpen(WB::passBuf(), WB::passCap(), true, s_kbdTitle, afterPass);
  }else M.popTo(&pgWifi);
}

static ConnectPage s_connect;
Page& pgConnect = s_connect;

/*  =================== клавіатура =================== */
static const char* KB[3][4] = {
  { "1234567890", "qwertyuiop", "asdfghjkl",  "zxcvbnm"  },
  { "1234567890", "QWERTYUIOP", "ASDFGHJKL",  "ZXCVBNM"  },
  { "1234567890", "!@#$%^&*()", "-_=+[]{}|?", "'\",.;:/" },
};
enum : int16_t { K_SHIFT = 64, K_BACK, K_PAGE, K_SPACE, K_OK, K_EYE };

class KbdPage : public Page {
  public:
    const char* title() override { return _title ? _title : ""; }
    bool scrollable() override { return false; }
    bool keepOpen() override { return true; }
    void draw(Gfx& g) override;
    void tick(uint32_t now) override;
    int16_t hit(int16_t x, int16_t y, Rect& r, uint8_t& radius) override;
    uint8_t grab(int16_t id) override { return id >= 0 ? 2 : 0; }
    void drag(int16_t id, int16_t x, int16_t y, bool end) override;
    void value(int16_t id, int32_t v) override;
    void back() override { _finish(false); }
    char* target = nullptr; size_t max = 0; bool pass = false; bool show = true;
    const char* _title = nullptr;
    void (*done)(bool) = nullptr;
    char undo[48] = { 0 };
    uint8_t page = 0;
    volatile int16_t down = -1;         /* клавіша під пальцем */
    uint32_t repT = 0, downT = 0; bool rep = false;
  private:
    /*  Клавіші: на 320×240 — 32×32, на 480×320 (ES3C35P) — 46×46: палець той самий,
        а екран більший — більші й клавіші. Нижній ряд ділиться в тих самих пропорціях.  */
    static const int16_t KY = 42, KH = (SH >= 320 ? 46 : 32), KW = (SW >= 480 ? 46 : 32);
    static const int16_t KB_PAGE = (SW * 60) / 320, KB_SPACE_X = (SW * 64) / 320, KB_OK_X = (SW * 216) / 320;
    bool _rect(int16_t k, Rect& r) const;
    int16_t _keyAt(int16_t x, int16_t y) const;
    void _finish(bool ok);
    uint32_t _sig = 0;
};

bool KbdPage::_rect(int16_t k, Rect& r) const {
  const int16_t by = KY + 4 * KH;
  switch(k){
    case K_SHIFT: r = Rect(2, KY + 3 * KH, KW + 12, KH); return true;
    case K_BACK:  r = Rect(SW - KW - 14, KY + 3 * KH, KW + 12, KH); return true;
    case K_PAGE:  r = Rect(2, by, KB_PAGE, KH); return true;
    case K_SPACE: r = Rect(KB_SPACE_X, by, KB_OK_X - KB_SPACE_X - 2, KH); return true;
    case K_OK:    r = Rect(KB_OK_X, by, SW - KB_OK_X - 2, KH); return true;
    case K_EYE:   r = Rect(SW - 52, 2, 44, 34); return true;
  }
  if(k < 0 || k >= 64) return false;
  uint8_t row = k >> 4, c = k & 15;
  const char* s = KB[page][row];
  uint8_t len = strlen(s);
  if(c >= len) return false;
  int16_t kw = KW, x0;
  if(row == 3){ const int16_t side = KW + 16; kw = (SW - 2 * side) / len; if(kw > KW) kw = KW; x0 = side + (SW - 2 * side - kw * len) / 2; }
  else x0 = (SW - len * KW) / 2;
  r = Rect(x0 + c * kw, KY + row * KH, kw, KH);
  return true;
}

int16_t KbdPage::_keyAt(int16_t x, int16_t y) const {
  if(y < KY){ return (pass && x >= SW - 56) ? K_EYE : -1; }
  int row = (y - KY) / KH; if(row > 4) row = 4;
  if(row == 4) return x < KB_SPACE_X - 1 ? K_PAGE : x < KB_OK_X - 1 ? K_SPACE : K_OK;
  if(row == 3 && x < KW + 15) return K_SHIFT;
  if(row == 3 && x >= SW - KW - 15) return K_BACK;
  const char* s = KB[page][row];
  uint8_t len = strlen(s);
  Rect r0;
  _rect(row * 16, r0);
  int c = (x - r0.x) / r0.w;
  if(c < 0) c = 0; if(c >= len) c = len - 1;
  return row * 16 + c;
}

void KbdPage::draw(Gfx& g){
  /*  рядок вводу  */
  if(g.visible(0, 0, SW, KY)){
    int16_t fw = pass ? SW - 64 : SW - 12;
    g.box(6, 2, fw, 34, R_BTN, C_SURF);
    char shown[64];
    size_t n = target ? strlen(target) : 0;
    if(pass && !show){ if(n > 40) n = 40; memset(shown, '*', n); shown[n] = 0; }
    else snprintf(shown, sizeof(shown), "%s", target ? target : "");
    /*  довгий — показуємо кінець  */
    const char* v = shown;
    while(*v && Gfx::textW(v, F_KEY) > fw - 28){ v++; while((*v & 0xC0) == 0x80) v++; }
    int16_t tw = g.text(16, 25, v, F_KEY, C_TXT);
    if((millis() / 500) % 2 == 0) g.fill(17 + tw, 10, 2, 18, C_ACC);
    if(pass){
      g.box(SW - 52, 2, 46, 34, R_BTN, show ? C_ACC : C_SURF);
      icon(g, IC_EYE, SW - 29, 19, show ? C_ACCTXT : C_TXT2, show ? C_ACC : C_SURF);
      if(!show) g.line(SW - 38, 28, SW - 20, 10, 1.8f, C_TXT2);
    }
  }
  /*  клавіші  */
  for(int16_t k = 0; k <= K_OK; k++){
    Rect r;
    if(!_rect(k, r) || !g.visible(r.x, r.y, r.w, r.h)) continue;
    bool dn = down == k, ok = k == K_OK;
    uint16_t bg = dn ? C_ACC : (ok ? C_ACC : (k >= 64 ? C_SURF2 : C_SURF));
    uint16_t fg = (dn || ok) ? C_ACCTXT : C_TXT;
    g.box(r.x + 2, r.y + 2, r.w - 4, r.h - 4, 7, bg);
    char t[12];
    if(k < 64){ t[0] = KB[page][k >> 4][k & 15]; t[1] = 0; g.text(r.x + r.w / 2, r.y + r.h / 2 + 6, t, F_KEY, fg, AL_C); }
    else if(k == K_SHIFT) icon(g, IC_SHIFT, r.x + r.w / 2, r.y + r.h / 2, page == 1 ? C_ACC : fg, bg);
    else if(k == K_BACK) icon(g, IC_BACKSPACE, r.x + r.w / 2, r.y + r.h / 2, fg, bg);
    else{
      const char* l = k == K_PAGE ? (page == 2 ? "abc" : "?123") : k == K_SPACE ? "пробіл" : "Готово";
      g.text(r.x + r.w / 2, r.y + r.h / 2 + 4, l, F_SMB, fg, AL_C);
    }
  }
  /*  збільшена клавіша над пальцем  */
  Rect r;
  int16_t d = down;
  if(d >= 0 && d < 64 && _rect(d, r)){
    int16_t px = r.x + r.w / 2 - 22, py = r.y - 46;
    if(px < 2) px = 2; if(px > SW - 46) px = SW - 46;
    g.box(px, py, 44, 48, 10, C_ACC);
    g.box(px + 2, py + 2, 40, 44, 8, C_SURF);
    char t[2] = { KB[page][d >> 4][d & 15], 0 };
    g.text(px + 22, py + 36, t, F_POP, C_ACC, AL_C);
  }
}

void KbdPage::tick(uint32_t now){
  uint32_t s = (target ? strlen(target) : 0) * 131 + page * 7 + show * 3 + (now / 500) % 2 + (uint32_t)(down + 5) * 1009;
  if(target) for(const char* p = target; *p; p++) s = s * 31 + (uint8_t)*p;
  if(s != _sig){ _sig = s; M.invalAll(); }
  /*  «стерти», якщо тримати, стирає далі  */
  if(down == K_BACK && now - downT > 500 && now - repT > 110){ repT = now; rep = true; M.postValue(K_BACK, 2, true); }
}

int16_t KbdPage::hit(int16_t x, int16_t y, Rect& r, uint8_t& radius){
  int16_t k = _keyAt(x, y);
  if(k < 0) return -1;
  if(!_rect(k, r)) return -1;
  radius = 7;
  return k;
}

void KbdPage::drag(int16_t id, int16_t x, int16_t y, bool end){
  (void)id;
  if(end){
    int16_t k = down;
    down = -1;
    if(k >= 0 && !(k == K_BACK && rep)) M.postValue(k, 1, true);
    rep = false;
    M.invalAll();
    return;
  }
  int16_t k = _keyAt(x, y);
  Rect r;
  /*  палець переходить на сусідню клавішу, лише вийшовши за межі поточної на 5 пікселів  */
  if(down >= 0 && _rect(down, r) && x >= r.x - 5 && x < r.x + r.w + 5 && y >= r.y - 5 && y < r.y + r.h + 5) return;
  if(k != down){ down = k; downT = millis(); rep = false; M.invalAll(); }
}

void KbdPage::_finish(bool ok){
  if(!ok && target) strlcpy(target, undo, max);
  void (*d)(bool) = done;
  done = nullptr;
  M.pop();
  if(d) d(ok);
}

void KbdPage::value(int16_t k, int32_t v){
  (void)v;
  if(!target) return;
  size_t l = strlen(target);
  if(k >= 0 && k < 64){
    char ch = KB[page][k >> 4][k & 15];
    if(ch && l + 1 < max){ target[l] = ch; target[l + 1] = 0; }
    return;
  }
  switch(k){
    case K_BACK:  if(l > 0){ l--; while(l > 0 && (target[l] & 0xC0) == 0x80) l--; target[l] = 0; } break;
    case K_SPACE: if(l + 1 < max){ target[l] = ' '; target[l + 1] = 0; } break;
    case K_SHIFT: page = page == 1 ? 0 : 1; break;
    case K_PAGE:  page = page == 2 ? 0 : 2; break;
    case K_EYE:   show = !show; break;
    case K_OK:    _finish(true); break;
  }
}

static KbdPage s_kbd;
Page& pgKbd = s_kbd;

void kbdOpen(char* target, size_t max, bool password, const char* title, void (*done)(bool ok)){
  s_kbd.target = target; s_kbd.max = max; s_kbd.pass = password; s_kbd._title = title; s_kbd.done = done;
  s_kbd.show = true; s_kbd.page = 0; s_kbd.down = -1;
  strlcpy(s_kbd.undo, target ? target : "", sizeof(s_kbd.undo));
  if(M.ltop() == &s_kbd) M.replace(&s_kbd);
  else M.push(&s_kbd);
}

}  // namespace m2
