#include "../core/options.h"
#include "m2player.h"
#include "m2pages.h"
#include "m2lang.h"
#include <SPIFFS.h>
#include <WiFi.h>
#include "esp_heap_caps.h"
#include "../core/config.h"
#include "../core/player.h"
#include "../core/network.h"
#include "../core/display.h"
#include "../core/timekeeper.h"
#include "../displays/dspcore.h"
#include "../displays/tools/spidma.h"
#include "../displays/tools/lcdout.h"
#include "m2simd.h"
#include "../extras/yoExtras.h"
#include "../extras/yoDlna.h"
#include "../extras/yoRecorder.h"
#include "../extras/yoSermons.h"
#include "../extras/yoLogos.h"
#include "../extras/yoMic.h"
#include "../extras/yoSpectrum.h"
#include "../extras/yoOta.h"
#include "../extras/yoVersion.h"
#include "../menu/yoMenu.h"

extern DspCore dsp;
extern float m2SpecDbg[32];

#ifdef YO_DEBUG
extern volatile bool yoVolTrace;          /* menu/yoDebug.cpp, команда voltrace */
#endif
namespace m2 {

Player P;
}
volatile bool g_dspWatch = false;
volatile bool g_m2Draw = false;
float m2SpecDbg[32] = { 0 };
namespace m2 {

static const int32_t STRIP = (int32_t)PSW * (LCD_PUSH_COPIES ? 16 : 32);   /* як у меню (m2ui.cpp), пікселі панелі */
/*  розкладка  */
static const int16_t TOP_H = 38;
static const int16_t CARD_Y = 42, CARD_H = 74;
static const int16_t CLK_Y = 120, CLK_H = 60;
static const int16_t ROW_Y = 182, ROW_H = 26;
static const int16_t VOL_Y = 212, VOL_H = 28;
static const int16_t LOGO = 58;

static uint32_t crcs(const char* s){
  uint32_t c = 0xFFFFFFFF;
  for(; *s; s++){ c ^= (uint8_t)*s; for(uint8_t k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & (0 - (c & 1))); }
  return ~c;
}
static uint32_t mixs(uint32_t h, const char* s){ if(s) for(; *s; s++) h = (h ^ (uint8_t)*s) * 16777619UL; return h; }


void Player::_mark(int16_t x, int16_t y, int16_t w, int16_t h){
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

void Player::show(){
  _shown = true;
  _sTop = _sCard = _sClock = _sSec = _sRow = _sVol = 0;
  _logoKey = 0xFFFFFFFF; _favKey = 0xFFFFFFFF;
  _t0Name = _t0Title = millis();
  invalAll();
}

/*  ---------- тло: темний перехід і світіння кольором логотипа ---------- */
void Player::_makeBg(uint16_t glow){
  if(!_bg) _bg = (uint16_t*)heap_caps_malloc((size_t)PSW * PSH * 2, MALLOC_CAP_SPIRAM);   /* пікселі панелі */
  if(!_bg) return;
  _bgColor = glow;
  static const uint8_t BAY[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
  const int br = ((C_BG >> 11) & 31) * 255 / 31, bgc = ((C_BG >> 5) & 63) * 255 / 63, bb = (C_BG & 31) * 255 / 31;
  const int tr = ((C_BGTOP >> 11) & 31) * 255 / 31, tg = ((C_BGTOP >> 5) & 63) * 255 / 63, tb = (C_BGTOP & 31) * 255 / 31;
  const int gr = ((glow >> 11) & 31) * 255 / 31, gg = ((glow >> 5) & 63) * 255 / 63, gb = (glow & 31) * 255 / 31;
  const float iK = (float)UI_KD / UI_KN;               /* піксель панелі → логічний */
  for(int16_t y = 0; y < PSH; y++){
    const float ly = y * iK;
    float ky = ly < 110 ? 1 - ly / 110.0f : 0;
    for(int16_t x = 0; x < PSW; x++){
      float dx = (x * iK - 50) / 190.0f, dy = (ly - 30) / 150.0f;
      float d = 1 - (dx * dx + dy * dy); if(d < 0) d = 0;
      float a = d * d * 0.32f;
      float r = br + (tr - br) * ky, g2 = bgc + (tg - bgc) * ky, b = bb + (tb - bb) * ky;
      r += (gr - r) * a; g2 += (gg - g2) * a; b += (gb - b) * a;
      int dd = BAY[((y & 3) << 2) | (x & 3)];
      int rr = ((int)r * 31 + dd * 16) / 255, g6 = ((int)g2 * 63 + dd * 16) / 255, b5 = ((int)b * 31 + dd * 16) / 255;
      if(rr > 31) rr = 31; if(g6 > 63) g6 = 63; if(b5 > 31) b5 = 31;
      _bg[(int32_t)y * PSW + x] = (uint16_t)((rr << 11) | (g6 << 5) | b5);
    }
  }
}

/*  ---------- логотип станції ---------- */
void Player::_loadLogo(){
  uint32_t key = player.remoteStationName ? 0xC0FE0000 + sermons.playing() : (config.getMode() == PM_SDCARD ? 0x5D : crcs(config.station.url));
  uint32_t full = key ^ (logos.version() * 2654435761UL);        /* логотип докачався — перечитати */
  if(full == _logoKey) return;
  _logoKey = full;
  _logoOk = false;
  uint16_t glow = RGB(40, 70, 110);
  if(!player.remoteStationName && config.getMode() != PM_SDCARD){
    char path[28]; snprintf(path, sizeof(path), "/logo/%08x.565", (unsigned)key);
    /*  робочий буфер — у PSRAM: внутрішньої пам'яті мало (від неї залежать TLS і Wi-Fi)  */
    static uint16_t* raw = (uint16_t*)heap_caps_malloc(LOGO_S * LOGO_S * 2, MALLOC_CAP_SPIRAM);
    if(!raw) return;
    bool ok = false;
    if(SPIFFS.exists(path)){
      File f = SPIFFS.open(path, "r");
      if(f && f.size() == LOGO_S * LOGO_S * 2) ok = f.read((uint8_t*)raw, LOGO_S * LOGO_S * 2) == LOGO_S * LOGO_S * 2;
      if(f) f.close();
    }else if(config.station.url[0]){
      /*  логотипа ще нема — попросити пошук (як і старий плеєр), якщо минулого разу не позначили «нема»  */
      /*  «нема» — тепер список у пам'яті (logos.isNo), а не файл на кожну станцію.
          Не почалось (пошук іншого логотипа ще йде, мережа не готова) — спробуємо
          ще раз за кілька секунд, а не лише коли зміниться станція  */
      if(!logos.isNo(key) && !logos.want(config.station.url, config.station.name)) _logoRetryT = millis() | 1;
    }
    if(ok){
      if(!_logo) _logo = (uint16_t*)heap_caps_malloc(LOGO * LOGO * 2, MALLOC_CAP_SPIRAM);
      if(_logo){
        /*  45 → 58: білінійно, щоб не було сходинок  */
        uint32_t sr = 0, sg = 0, sb = 0, n = 0;
        for(int16_t y = 0; y < LOGO; y++){
          float fy = y * (LOGO_S - 1) / (float)(LOGO - 1); int y0 = (int)fy; int y1 = y0 + 1 < LOGO_S ? y0 + 1 : y0; float ay = fy - y0;
          for(int16_t x = 0; x < LOGO; x++){
            float fx = x * (LOGO_S - 1) / (float)(LOGO - 1); int x0 = (int)fx; int x1 = x0 + 1 < LOGO_S ? x0 + 1 : x0; float ax = fx - x0;
            uint16_t p00 = raw[y0 * LOGO_S + x0], p01 = raw[y0 * LOGO_S + x1], p10 = raw[y1 * LOGO_S + x0], p11 = raw[y1 * LOGO_S + x1];
            auto ch = [&](int sh, int m){
              float a = ((p00 >> sh) & m) * (1 - ax) + ((p01 >> sh) & m) * ax;
              float b = ((p10 >> sh) & m) * (1 - ax) + ((p11 >> sh) & m) * ax;
              return (int)lroundf(a * (1 - ay) + b * ay);
            };
            int r = ch(11, 31), g = ch(5, 63), b = ch(0, 31);
            _logo[y * LOGO + x] = (uint16_t)((r << 11) | (g << 5) | b);
            sr += r; sg += g; sb += b; n++;
          }
        }
        _logoOk = true;
        /*  колір світіння — середній колір логотипа, трохи насичений  */
        int r = sr / n * 255 / 31, g = sg / n * 255 / 63, b = sb / n * 255 / 31;
        int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
        if(mx > 20){ r = r * 200 / mx; g = g * 200 / mx; b = b * 200 / mx; }
        glow = RGB(r, g, b);
      }
    }else{
      static const uint16_t PAL[8] = { 0x3A8D, 0x5A4B, 0x2C6A, 0x6A28, 0x2B0F, 0x7A6C, 0x4B09, 0x31CC };
      glow = PAL[key & 7];
    }
  }else if(player.remoteStationName) glow = RGB(110, 70, 160);
  else glow = RGB(160, 100, 40);
  _glow = glow;
  if(!_bg || glow != _bgColor){ _makeBg(glow); invalAll(); }
}

void Player::_loadFav(){
  uint32_t key = 17;
  for(uint8_t i = 0; i < 6; i++) key = key * 31 + crcs(extras.fav[i].url);
  if(key == _favKey) return;
  _favKey = key;
  static uint16_t* raw = (uint16_t*)heap_caps_malloc(LOGO_S * LOGO_S * 2, MALLOC_CAP_SPIRAM);
  if(!raw) return;
  for(uint8_t i = 0; i < 6; i++){
    _favOk[i] = false;
    if(!extras.fav[i].url[0]) continue;
    char path[28]; snprintf(path, sizeof(path), "/logo/%08x.565", (unsigned)crcs(extras.fav[i].url));
    if(!SPIFFS.exists(path)) continue;
    File f = SPIFFS.open(path, "r");
    bool ok = f && f.size() == LOGO_S * LOGO_S * 2 && f.read((uint8_t*)raw, LOGO_S * LOGO_S * 2) == LOGO_S * LOGO_S * 2;
    if(f) f.close();
    if(!ok) continue;
    if(!_favPix[i]) _favPix[i] = (uint16_t*)heap_caps_malloc(24 * 24 * 2, MALLOC_CAP_SPIRAM);
    if(!_favPix[i]) continue;
    for(int16_t y = 0; y < 24; y++) for(int16_t x = 0; x < 24; x++){
      /*  45 → 24: середнє по квадрату  */
      int sx = x * LOGO_S / 24, sy = y * LOGO_S / 24, r = 0, g = 0, b = 0, n = 0;
      for(int dy = 0; dy < 2; dy++) for(int dx = 0; dx < 2; dx++){
        uint16_t p = raw[(sy + dy < LOGO_S ? sy + dy : sy) * LOGO_S + (sx + dx < LOGO_S ? sx + dx : sx)];
        r += (p >> 11) & 31; g += (p >> 5) & 63; b += p & 31; n++;
      }
      _favPix[i][y * 24 + x] = (uint16_t)(((r / n) << 11) | ((g / n) << 5) | (b / n));
    }
    _favOk[i] = true;
  }
  _mark(0, ROW_Y, SW, ROW_H);
}

static bool sermonOn(){ return player.remoteStationName && sermons.playing() >= 0; }

#ifdef YO_DEBUG
volatile uint32_t yoRowDur = 0, yoRowPos = 0, yoRowN = 0;   /* відладка: що бачить задача екрана */
#endif

uint8_t Player::_mode() const {
  if(config.getMode() == PM_SDCARD || sermonOn()) return 2;
  bool any = false;
  for(uint8_t i = 0; i < 6; i++) if(extras.fav[i].url[0]) any = true;
  if(!extras.s.favHide && any) return 1;
  return 0;
}

/*  ---------- стан у шапці ----------
    Значки стану стоять справа наліво з однаковим проміжком. Ширини описані
    тут один раз: за ними і рахується вільне місце під назву станції, і
    малюється сама смуга (_drawTop), тож вони не наповзають одне на одне.  */
static const int16_t ST_RIGHT = SW - 44;   /*  правий край смуги стану  */
static const int16_t ST_GAP   = 7;         /*  проміжок між значками  */
static const int16_t W_WIFI = 16, W_BAT = 21, W_BOLT = 11, W_MIC = 11, W_BELL = 14;
static const int16_t W_MOON = 13 + 3 + 20; /*  значок + число хвилин  */
static const int16_t W_REC  =  9 + 3 + 22; /*  крапка + хвилини запису  */

int16_t Player::_statusLeft() const {
  int16_t x = ST_RIGHT - W_WIFI;
  if(extras.batMv() >= 2800 && !extras.s.noBat){
    x -= ST_GAP + W_BAT;
    if(extras.onPower()) x -= W_BOLT;                /*  блискавка притулена до батареї  */
  }
  if(mic.listening())    x -= ST_GAP + W_MIC;
  if(extras.s.alarmOn)   x -= ST_GAP + W_BELL;
  if(extras.sleepLeft()) x -= ST_GAP + W_MOON;
  if(recorder.active())  x -= ST_GAP + W_REC;
  return x;
}

/*  Значки стану малюємо тут, а не беремо з набору меню: ті розраховані на
    рядок налаштувань (18–19 пікселів заввишки), і поряд із батареєю (12)
    виглядали завеликими та чужорідними. Ці — в одну висоту з батареєю.  */
static void stBell(Gfx& g, float cx, float cy, uint16_t c){
  g.circle(cx, cy - 2.4f, 4.2f, c);
  const float xy[8] = { cx - 4.2f, cy - 2.4f, cx + 4.2f, cy - 2.4f, cx + 5.2f, cy + 2.6f, cx - 5.2f, cy + 2.6f };
  g.poly(xy, 4, c);
  g.line(cx - 5.8f, cy + 2.9f, cx + 5.8f, cy + 2.9f, 1.6f, c);
  g.circle(cx, cy + 5.2f, 1.5f, c);
}
static void stMic(Gfx& g, float cx, float cy, uint16_t c){
  g.box((int16_t)(cx - 2.5f), (int16_t)(cy - 6.5f), 5, 9, 2, c);
  g.arc(cx, cy - 1.0f, 4.6f, 1.6f, c, 95, 265);
  g.line(cx, cy + 3.6f, cx, cy + 6.2f, 1.6f, c);
}
static void stMoon(Gfx& g, float cx, float cy, uint16_t c, uint16_t bg){
  g.circle(cx, cy, 6.3f, c);
  g.circle(cx + 3.7f, cy - 2.9f, 5.5f, bg);
}

static const char* const WDAY[7] = { "Неділя", "Понеділок", "Вівторок", "Середа", "Четвер", "П'ятниця", "Субота" };
static const char* const MON[12] = { "січня", "лютого", "березня", "квітня", "травня", "червня", "липня", "серпня", "вересня", "жовтня", "листопада", "грудня" };

void Player::_drawTop(Gfx& g, uint32_t now){
  if(!g.visible(0, 0, SW, TOP_H)) return;
  /*  джерело  */
  g.circle(20, 19, 14, C_SURF);
  uint8_t src = (dlna.playing() || player.extOn) ? IC_SPEAKER : (player.remoteStationName ? IC_BACK : (config.getMode() == PM_SDCARD ? IC_CARD : IC_RADIO));
  icon(g, src, 20, 19, C_ACC, C_SURF);
  /*  меню  */
  g.circle(SW - 20, 19, 14, C_SURF);
  icon(g, IC_MENU, SW - 20, 19, C_TXT, C_SURF);
  /*  стан — праворуч наліво, кроками з таблиці ширин вище  */
  int16_t x = ST_RIGHT - W_WIFI;
  int rs = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -127;
  uint8_t wl = rs > -55 ? 4 : rs > -65 ? 3 : rs > -75 ? 2 : rs > -85 ? 1 : 0;
  signalBars(g, x, 25, wl, C_TXT2, C_SURF2);
  char b[16];
  if(extras.batMv() >= 2800 && !extras.s.noBat){
    x -= ST_GAP + W_BAT;
    int8_t pct = extras.batPct(); if(pct < 0) pct = 0;
    /*  Колір — завжди за станом, а не лише поки заряджається: інакше, щойно
        батарея дозарядилась (живлення є, «заряджено»), значок сірів.
        Від зарядника — зелений; від батареї — за рівнем.  */
    bool low = extras.lowBattery(), pwr = extras.onPower();
    uint16_t c = pwr ? C_GREEN : (low || pct < 10 ? C_RED : (pct < 30 ? C_ORANGE : C_GREEN));
    const int16_t bx = x;
    if(pwr){
      /*  підключено до зарядника / комп'ютера — блискавка перед батареєю  */
      x -= W_BOLT;
      static const float BOLT[] = { 5.5f, 0, 0.5f, 7, 3.6f, 7, 2.5f, 12, 7.5f, 5, 4.4f, 5 };
      float pts[12];
      for(uint8_t k = 0; k < 12; k += 2){ pts[k] = x + 1 + BOLT[k]; pts[k + 1] = 13 + BOLT[k + 1]; }
      g.poly(pts, 6, C_GREEN);
    }
    g.frame(bx, 13, 19, 12, 3, c, 1);
    g.box(bx + 19, 16, 2, 6, 1, c);
    int16_t fw = (int16_t)(15 * pct / 100); if(fw < 1) fw = 1;
    g.box(bx + 2, 15, fw, 8, 2, c);
  }
  if(mic.listening()){  x -= ST_GAP + W_MIC;  stMic(g, x + 5.5f, 19, C_TXT2); }
  if(extras.s.alarmOn){ x -= ST_GAP + W_BELL; stBell(g, x + 7, 19, C_TXT2); }
  if(extras.sleepLeft()){
    x -= ST_GAP + W_MOON; stMoon(g, x + 6.5f, 19, C_TXT2, C_BG);
    snprintf(b, sizeof(b), "%u", (unsigned)extras.sleepLeft());
    g.text(x + 16, 24, b, F_SMB, C_TXT2);
  }
  if(recorder.active()){
    x -= ST_GAP + W_REC; g.circle(x + 4.5f, 19, 4.5f, C_REC);
    snprintf(b, sizeof(b), "%u'", (unsigned)(recorder.seconds() / 60));
    g.text(x + 12, 24, b, F_SMB, C_REC);
  }
  /*  назва станції: не влазить — біжить  */
  const int16_t nx = 42, nw = x - 8 - nx;
  const char* name = config.station.name;
  if(player.remoteStationName && sermons.playing() >= 0 && sermons.at(sermons.playing())) name = "Проповідь";
  else if(player.extOn) name = "AirPlay";                     /* хто грає і що — у картці нижче */
  else if(dlna.playing()) name = "Бездротова колонка";        /* назва доріжки — у картці нижче */
  int16_t tw = Gfx::textW(name, F_TITLE);
  Rect s = g.narrow(nx, 0, nw, TOP_H);
  if(tw <= nw) g.text(nx, 25, name, F_TITLE, C_TXT);
  else{
    int16_t off = _offName;
    g.text(nx - off, 25, name, F_TITLE, C_TXT);
    g.text(nx - off + tw + 40, 25, name, F_TITLE, C_TXT);
  }
  g.restore(s);
}

/*  Що писати в картці: назва (жирно) і виконавець.  */
static void cardLines(char* line1, size_t c1, char* line2, size_t c2){
  line1[0] = line2[0] = 0;
  bool playing = player.status() == PLAYING;
  if(config.station.title[0]){
    const char* dash = strstr(config.station.title, " - ");
    if(dash){
      strlcpy(line1, dash + 3, c1);
      size_t n = dash - config.station.title; if(n >= c2) n = c2 - 1;
      memcpy(line2, config.station.title, n); line2[n] = 0;
    }else strlcpy(line1, config.station.title, c1);
  }else{
    strlcpy(line1, playing ? "Грає" : "Зупинено", c1);
    strlcpy(line2, config.station.name, c2);
  }
}

/*  Текст у кілька рядків по словах; повертає, скільки рядків вийшло.  */
static uint8_t wrapText(Gfx& g, int16_t x, int16_t y, int16_t w, int16_t lh, uint8_t maxLines, const char* s, const GFXfont* f, uint16_t c){
  char line[96]; uint8_t n = 0;
  while(*s && n < maxLines){
    while(*s == ' ') s++;
    const char* best = nullptr; const char* p = s;
    char tmp[96];
    while(*p){
      const char* sp = strchr(p, ' ');
      const char* e = sp ? sp : p + strlen(p);
      size_t len = e - s; if(len >= sizeof(tmp)) break;
      memcpy(tmp, s, len); tmp[len] = 0;
      if(Gfx::textW(tmp, f) > w) break;
      best = e;
      if(!sp) break;
      p = sp + 1;
    }
    bool last = n + 1 == maxLines;
    if(!best || last){
      /*  останній рядок чи слово не влазить — ріжемо з «…»  */
      strlcpy(line, s, sizeof(line));
      g.text(x, y + n * lh, line, f, c, AL_L, w);
      return n + 1;
    }
    size_t len = best - s; if(len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, s, len); line[len] = 0;
    g.text(x, y + n * lh, line, f, c);
    n++;
    s = best;
  }
  return n;
}

static const int16_t SM_H = 111;          /* проповідь: висота картки з великою обкладинкою */
int16_t Player::_cardH() const { return sermonOn() ? SM_H : CARD_H; }

void Player::_drawCard(Gfx& g, uint32_t now){
  if(sermonOn() && sermons.at(sermons.playing())){
    if(!g.visible(MX, CARD_Y, CWID, SM_H)) return;
    const Sermon* sm = sermons.at(sermons.playing());
    g.box(MX, CARD_Y, CWID, SM_H, 16, C_SURF);
    const int16_t cx = MX + 6, cy = CARD_Y + 6;
    const uint16_t* big = sermons.coverBig();
    if(big) g.image(cx, cy, COVERB_W, COVERB_H, big, 12);
    else{ g.box(cx, cy, COVERB_W, COVERB_H, 12, _glow); icon(g, IC_CROSS, cx + COVERB_W / 2, cy + COVERB_H / 2, 0xFFFF, _glow); }
    if(player.status() != PLAYING){
      g.circle(cx + COVERB_W / 2, cy + COVERB_H / 2, 20, C_ACC);
      icon(g, IC_PLAY, cx + COVERB_W / 2 + 1, cy + COVERB_H / 2, C_ACCTXT, C_ACC);
    }
    /*  Назва — до 4 рядків, під нею проповідник і дата по рядку (з «…»):
        раніше проповідник міг зайняти два рядки й наповзав на дату.  */
    const int16_t tx = cx + COVERB_W + 9, tw = MX + CWID - 8 - tx;
    uint8_t n = wrapText(g, tx, CARD_Y + 19, tw, 15, 4, sm->title, F_ROWB, C_TXT);
    int16_t yp = CARD_Y + 19 + (n ? n : 1) * 15 + 2;
    if(yp > CARD_Y + SM_H - 24) yp = CARD_Y + SM_H - 24;
    g.text(tx, yp, sm->preacher, F_SM, C_TXT2, AL_L, tw);
    g.text(tx, yp + 14, sm->date, F_SM, C_TXT3, AL_L, tw);
    return;
  }
  if(!g.visible(MX, CARD_Y, CWID, CARD_H)) return;
  g.box(MX, CARD_Y, CWID, CARD_H, 16, C_SURF);
  int16_t tx = MX + 76;
  const uint16_t* cover = player.remoteStationName ? sermons.coverPix() : nullptr;
  if(cover){
    g.image(MX + 8, CARD_Y + (CARD_H - COVER_H) / 2, COVER_W, COVER_H, cover, 8);
    tx = MX + 16 + COVER_W;
  }else if(_logoOk && _logo){
    g.image(MX + 8, CARD_Y + 8, LOGO, LOGO, _logo, 12);
  }else{
    g.box(MX + 8, CARD_Y + 8, LOGO, LOGO, 12, _glow);
    if(config.getMode() == PM_SDCARD) icon(g, IC_CARD, MX + 8 + LOGO / 2, CARD_Y + 8 + LOGO / 2, 0xFFFF, _glow);
    else{
      char ini[8] = { 0 }; const char* s = config.station.name; uint8_t k = 0, ch = 0;
      while(*s && ch < 2 && k < 6){ uint8_t c = (uint8_t)*s; uint8_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 1; if(c == ' ' || c == '*'){ s++; continue; } for(uint8_t j = 0; j < len && s[j]; j++) ini[k++] = s[j]; s += len; ch++; }
      g.text(MX + 8 + LOGO / 2, CARD_Y + 8 + LOGO / 2 + 8, ini, F_MID, 0xFFFF, AL_C);
    }
  }
  /*  що грає: «виконавець - назва» → назва жирно, виконавець нижче  */
  char line1[160], line2[160];
  line1[0] = line2[0] = 0;
  bool playing = player.status() == PLAYING;
  if(player.remoteStationName && sermons.playing() >= 0 && sermons.at(sermons.playing())){
    const Sermon* sm = sermons.at(sermons.playing());
    strlcpy(line1, sm->title, sizeof(line1));
    snprintf(line2, sizeof(line2), "%s · %s", sm->preacher, sm->date);
  }else cardLines(line1, sizeof(line1), line2, sizeof(line2));
  const int16_t right = MX + CWID - 44, w = right - tx;
  Rect s = g.narrow(tx, CARD_Y, w, CARD_H);
  int16_t t1 = Gfx::textW(line1, F_ROWB);
  if(t1 <= w) g.text(tx, CARD_Y + 26, line1, F_ROWB, C_TXT);
  else{
    int16_t off = _offTitle;
    g.text(tx - off, CARD_Y + 26, line1, F_ROWB, C_TXT);
    g.text(tx - off + t1 + 40, CARD_Y + 26, line1, F_ROWB, C_TXT);
  }
  g.restore(s);
  g.text(tx, CARD_Y + 44, line2, F_ROW, C_TXT2, AL_L, w);
  /*  бітрейт і кодек  */
  if(config.station.bitrate && playing){
    char b[32]; snprintf(b, sizeof(b), tr("%u кбіт/с · %s"), (unsigned)config.station.bitrate, player.getCodecname());
    int16_t bw = Gfx::textW(b, F_SM) + 16;
    if(bw > w) bw = w;
    g.box(tx, CARD_Y + 51, bw, 16, 8, C_SURF2);
    g.text(tx + 8, CARD_Y + 63, b, F_SM, C_TXT2, AL_L, bw - 12);
  }
  /*  праворуч: грає — живі риски; стоїть — кнопка «грати»  */
  const float cx = MX + CWID - 24, cy = CARD_Y + CARD_H / 2;
  if(playing){
    for(uint8_t k = 0; k < 5; k++){
      float h = 4 + _bars[k] * 18;
      g.line(cx - 10 + k * 5, cy + 10, cx - 10 + k * 5, cy + 10 - h, 3, C_ACC);
    }
  }else{
    g.circle(cx, cy, 16, C_ACC);
    icon(g, IC_PLAY, cx + 1, cy, C_ACCTXT, C_ACC);
  }
}

void Player::_drawClock(Gfx& g){
  if(!g.visible(0, CLK_Y, SW, CLK_H)) return;
  const struct tm& t = network.timeinfo;
  if(sermonOn() && sermons.at(sermons.playing())){
    /*  під великою обкладинкою — годинник і дата одним рядком  */
    if(t.tm_year < 120) return;
    char b[48];
    snprintf(b, sizeof(b), "%02d:%02d", t.tm_hour, t.tm_min);
    int16_t w = g.text(MX + 4, CARD_Y + SM_H + 24, b, F_MID, C_TXT);
    snprintf(b, sizeof(b), "%s, %d %s", tr(WDAY[t.tm_wday % 7]), t.tm_mday, tr(MON[t.tm_mon % 12]));
    g.text(MX + 12 + w, CARD_Y + SM_H + 24, b, F_ROW, C_TXT2, AL_L, SW - MX - 20 - w);
    return;
  }
  bool ok = t.tm_year >= 120;
  char b[48];                                   /* кирилиця — по два байти на літеру */
  if(ok) snprintf(b, sizeof(b), "%02d:%02d", t.tm_hour, t.tm_min); else snprintf(b, sizeof(b), "--:--");
  int16_t w = g.text(12, CLK_Y + 48, b, F_CLOCK, C_TXT);
  if(ok){ snprintf(b, sizeof(b), "%02d", t.tm_sec); g.text(18 + w, CLK_Y + 48, b, F_TITLE, C_TXT2); }
  const int16_t rx = SW - 14;
  if(ok){
    g.text(rx, CLK_Y + 12, WDAY[t.tm_wday % 7], F_ROWB, C_TXT, AL_R);
    snprintf(b, sizeof(b), "%d %s", t.tm_mday, tr(MON[t.tm_mon % 12]));
    g.text(rx, CLK_Y + 28, b, F_ROW, C_TXT2, AL_R);
  }
  if(timekeeper.weatherHave){
    snprintf(b, sizeof(b), "%d°", (int)lroundf(timekeeper.weatherTemp));
    int16_t tw = g.text(rx, CLK_Y + 52, b, F_TITLE, C_TXT, AL_R);
    /*  значок погоди  */
    /*  Значок погоди був заввишки майже як два рядки тексту й упирався в дату.
        Тепер менший (S) і має власне сонце: те, що з набору меню, розраховане
        на рядок налаштувань і поряд із написом виглядало завеликим.  */
    const float S = 0.8f;
    float cx = rx - tw - 17, cy = CLK_Y + 45;
    uint8_t ic = timekeeper.weatherIcon;
    const uint16_t cl = RGB(205, 210, 220), sun = C_ACC;
    auto sunny = [&](float x, float y, float s, uint16_t c){
      g.circle(x, y, 3.6f * s, c);
      for(uint8_t k = 0; k < 8; k++){
        float a = k * 0.7854f;
        g.line(x + 5.9f * s * cosf(a), y + 5.9f * s * sinf(a), x + 8.0f * s * cosf(a), y + 8.0f * s * sinf(a), 1.7f, c);
      }
    };
    auto cloud = [&](float x, float y, float s, uint16_t c){
      g.circle(x - 4 * s, y + 1 * s, 4.2f * s, c); g.circle(x + 2 * s, y - 1.5f * s, 5.5f * s, c);
      g.box((int16_t)(x - 8 * s), (int16_t)(y + 1 * s), (int16_t)(16 * s), (int16_t)(5 * s), (uint8_t)(2.5f * s), c);
    };
    if(ic == 0){ sunny(cx, cy - 1, S, sun); }
    else if(ic == 1){ sunny(cx + 3 * S, cy - 5 * S, S * 0.85f, sun); cloud(cx - 2 * S, cy + 1 * S, 0.9f * S, cl); }
    else if(ic == 2 || ic == 3){ if(ic == 3) cloud(cx + 5 * S, cy - 4 * S, 0.7f * S, RGB(140, 148, 160)); cloud(cx, cy, S, cl); }
    else if(ic == 4 || ic == 5){ cloud(cx, cy - 3 * S, S, cl); for(uint8_t k = 0; k < 3; k++) g.line(cx + (-5 + k * 5) * S, cy + 5 * S, cx + (-7 + k * 5) * S, cy + 10 * S, 1.6f, C_BLUE); }
    else if(ic == 6){ cloud(cx, cy - 3 * S, S, cl); const float xy[8] = { cx + 1 * S, cy + 3 * S, cx - 4 * S, cy + 10 * S, cx - 1 * S, cy + 10 * S, cx - 3 * S, cy + 15 * S }; g.line(xy[0], xy[1], xy[2], xy[3], 2, sun); g.line(xy[2], xy[3], xy[4], xy[5], 2, sun); g.line(xy[4], xy[5], xy[6], xy[7], 2, sun); }
    else if(ic == 7){ cloud(cx, cy - 3 * S, S, cl); for(uint8_t k = 0; k < 3; k++) g.circle(cx + (-5 + k * 5) * S, cy + 8 * S, 1.5f, 0xFFFF); }
    else if(ic == 8){ for(uint8_t k = 0; k < 3; k++) g.line(cx + (-8 + (k & 1) * 3) * S, cy + (-4 + k * 5) * S, cx + (8 - (k & 1) * 3) * S, cy + (-4 + k * 5) * S, 2, cl); }
  }
}

void Player::_drawRow(Gfx& g){
  if(!g.visible(0, ROW_Y, SW, ROW_H)) return;
  uint8_t m = _mode();
  if(m == 1){
    int8_t pl = extras.favPlaying();
    for(uint8_t i = 0; i < 6; i++){
      float cx = 30 + i * ((SW - 60) / 5.0f), cy = ROW_Y + 13;   /* шість кружків на всю ширину */
      if(!extras.fav[i].url[0]){ g.arc(cx, cy, 12, 1.2f, C_LINE); icon(g, IC_PLUS, cx, cy, C_TXT3, C_BG); continue; }
      if(pl == (int8_t)i) g.circle(cx, cy, 15, C_ACC);
      if(_favOk[i] && _favPix[i]){ g.circle(cx, cy, 13, C_SURF); g.image((int16_t)cx - 12, (int16_t)cy - 12, 24, 24, _favPix[i], 12); }
      else{
        static const uint16_t PAL[8] = { 0x3A8D, 0x5A4B, 0x2C6A, 0x6A28, 0x2B0F, 0x7A6C, 0x4B09, 0x31CC };
        g.circle(cx, cy, 13, PAL[crcs(extras.fav[i].url) & 7]);
        char ini[4] = { 0 }; const char* s = extras.fav[i].name; uint8_t k = 0;
        uint8_t c = (uint8_t)*s; uint8_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : 3; for(uint8_t j = 0; j < len && s[j] && k < 3; j++) ini[k++] = s[j];
        g.text((int16_t)cx, (int16_t)cy + 5, ini, F_ROWB, 0xFFFF, AL_C);
      }
    }
    return;
  }
  if(m == 2){
    /*  пульт картки / проповіді: ⏮ смуга ⏭  */
    const float cy = ROW_Y + 13;
    g.circle(26, cy, 13, _btn == 0 ? C_ACC : C_SURF);
    g.line(20, cy - 6, 20, cy + 6, 2, _btn == 0 ? C_ACCTXT : C_TXT);
    { const float xy[6] = { 32, cy - 6, 32, cy + 6, 22, cy }; g.poly(xy, 3, _btn == 0 ? C_ACCTXT : C_TXT); }
    g.circle(SW - 26, cy, 13, _btn == 1 ? C_ACC : C_SURF);
    g.line(SW - 20, cy - 6, SW - 20, cy + 6, 2, _btn == 1 ? C_ACCTXT : C_TXT);
    { const float xy[6] = { SW - 32, cy - 6, SW - 32, cy + 6, SW - 22, cy }; g.poly(xy, 3, _btn == 1 ? C_ACCTXT : C_TXT); }
    uint32_t dur = player.durSec(), pos = player.posSec();
#ifdef YO_DEBUG
    { extern volatile uint32_t yoRowDur, yoRowPos, yoRowN; yoRowDur = dur; yoRowPos = pos; yoRowN++; }   /* відладка: що бачить задача екрана */
#endif
    float f = dur ? (float)pos / dur : 0;
    if(_seek >= 0) f = _seek;
    if(f > 1) f = 1;
    const int16_t bx = 76, bw = SW - 152;
    char b[12];
    uint32_t sh = _seek >= 0 ? (uint32_t)(f * dur) : pos;
    snprintf(b, sizeof(b), "%u:%02u", (unsigned)(sh / 60), (unsigned)(sh % 60));
    g.text(bx - 6, (int16_t)cy + 4, b, F_SM, C_TXT2, AL_R);
    snprintf(b, sizeof(b), "%u:%02u", (unsigned)(dur / 60), (unsigned)(dur % 60));
    g.text(bx + bw + 6, (int16_t)cy + 4, dur ? b : "--:--", F_SM, C_TXT2);
    drawSlider(g, bx, (int16_t)cy, bw, f, dur > 0);
    return;
  }
  /*  рівень звуку: риски, що дихають із музикою  */
  bool playing = player.status() == PLAYING;
  const float step = (SW - 28 - 4) / 31.0f;           /* 32 риски на всю ширину (на 320 — 9,3) */
  for(uint8_t k = 0; k < 32; k++){
    float h = _specH[k] > 2 ? _specH[k] : 2;
    float x = 14 + k * step;
    uint16_t c = playing ? Gfx::blend(C_SURF2, C_ACC, (uint8_t)(90 + _spec[k] * 165)) : C_SURF2;
    g.line(x, ROW_Y + 24, x, ROW_Y + 24 - h, 4.5f, c);
  }
}

void Player::_drawVol(Gfx& g){
  if(!g.visible(0, VOL_Y, SW, VOL_H)) return;
  int v = _volShownF >= 0 ? (int)lroundf(_volShownF) : (_volDrag >= 0 ? _volDrag : (int)config.store.volume);
  const int16_t cy = VOL_Y + 14;
  icon(g, IC_SPEAKER, 22, cy, C_TXT2, C_BG);
  drawSlider(g, 40, cy, SW - 40 - 56, v / 254.0f);
  char b[8]; snprintf(b, sizeof(b), "%d%%", (int)((v * 100 + 127) / 254));
  g.text(SW - 14, cy + 4, b, F_SMB, C_TXT, AL_R);
}

/*  Хвиля: за 280 мс розходиться від пальця на всю картку; після відпускання
    (але не раніше ніж через 200 мс від дотику — короткий дотик теж видно
    повністю) гасне за 260 мс.  */
void Player::_ripple(Gfx& g){
  if(!_rf.on) return;
  /*  як у списках (Menu::_ripStart): хвиля — через 90 мс або від відпускання, щоб жест
      угору-вниз (до списку станцій) не спалахував під пальцем  */
  const uint32_t st = (_rf.rel && (int32_t)(_rf.up - (_rf.t0 + 90)) < 0) ? _rf.up : _rf.t0 + 90;
  if((int32_t)(_frameT - st) < 0) return;
  const int32_t age = (int32_t)(_frameT - st);
  float k = age <= 0 ? 0 : age / 280.0f; if(k > 1) k = 1;
  const float rad = 10 + SW * (1 - (1 - k) * (1 - k) * (1 - k));    /* до найдальшого кута картки */
  float a = 50;
  if(_rf.rel){
    const uint32_t from = _rf.up > _rf.t0 + 200 ? _rf.up : _rf.t0 + 200;
    if(_frameT > from){ const float f = (_frameT - from) / 260.0f; a = f >= 1 ? 0 : 50 * (1 - f); }
  }
  if(a >= 1) g.lighten(Rect(MX, CARD_Y, CWID, _cardH()), 16, _rf.x, _rf.y, rad, (uint8_t)a);
}

void Player::_drawPopup(Gfx& g){
  Rect r = _popRect();
  if(!g.visible(r.x, r.y, r.w, r.h)) return;
  /*  тінь під карткою й сама картка  */
  g.fillA(0, 0, SW, SH, 0x0000, 90);
  g.box(r.x, r.y, r.w, r.h, 18, C_SURF);
  g.frame(r.x, r.y, r.w, r.h, 18, (_popup == 2 || _popup == 3) ? C_RED : C_ACC, 1);
  char b[96];
  if(_popup >= 3){
    /*  батарея / зв'язок / картка / оновлення файлом — без кнопок, крім батареї  */
    const bool bat = _popup == 3;
    const uint16_t col = bat ? C_RED : C_ACC;
    g.circle(r.x + 30, r.y + 32, 18, col);
    if(bat){
      /*  порожня батарея з одним «діленням»  */
      g.frame(r.x + 18, r.y + 25, 22, 14, 3, 0xFFFF, 2);
      g.box(r.x + 40, r.y + 29, 3, 6, 1, 0xFFFF);
      g.box(r.x + 21, r.y + 28, 4, 8, 1, 0xFFFF);
    }else icon(g, _popup == 4 ? IC_WIFI : (_popup == 5 ? IC_CARD : IC_REFRESH), r.x + 30, r.y + 32, C_ACCTXT, col);
    const char* t1 = "", *t2 = "", *t3 = "";
    if(bat){
      t1 = "Батарея сідає";
      snprintf(b, sizeof(b), tr("%d%% — під'єднайте зарядку"), extras.batPct());
      t2 = b; t3 = "нагадую щохвилини, поки не під'єднаєте";
    }else if(_popup == 4){
      t1 = "Немає зв'язку";
      t2 = "радіо саме підключається до мережі…"; t3 = "торкніться — вибрати іншу мережу";
    }else if(_popup == 5){
      t1 = "Картка пам'яті";
      if(_statusN >= 0){ snprintf(b, sizeof(b), tr("знайдено файлів: %ld"), (long)_statusN); t2 = b; } else t2 = "читаю список треків…";
      t3 = "за мить заграє";
    }else{
      t1 = "Оновлення"; t2 = "записую нову прошивку…"; t3 = "не вимикайте радіо";
    }
    g.text(r.x + 60, r.y + 39, t1, F_TITLE, C_TXT, AL_L, r.w - 74);
    g.text(r.x + 16, r.y + 78, t2, F_ROW, bat ? C_TXT : C_TXT2, AL_L, r.w - 32);
    g.text(r.x + 16, r.y + 98, t3, F_SM, C_TXT3, AL_L, r.w - 32);
    if(bat){
      const int16_t by = r.y + r.h - 46;
      g.box(r.x + 14, by, r.w - 28, 34, 12, _popBtn >= 0 ? C_LINE : C_SURF2);
      g.text(r.x + r.w / 2, by + 22, "Зрозуміло", F_ROWB, C_TXT, AL_C);
    }else if(_popup != 5){
      /*  іде робота — біжить крапка  */
      const float a = fmodf(_frameT * 0.36f, 360.0f);
      g.arc(r.x + r.w / 2, r.y + r.h - 28, 12, 3, C_SURF2);
      g.arc(r.x + r.w / 2, r.y + r.h - 28, 12, 3, C_ACC, a, a + 90);
    }
    return;
  }
  if(_popup == 1){
    g.circle(r.x + 30, r.y + 30, 16, C_ACC);
    icon(g, IC_DOWN, r.x + 30, r.y + 31, C_ACCTXT, C_ACC);
    g.text(r.x + 56, r.y + 26, "Є нова версія", F_TITLE, C_TXT, AL_L, r.w - 70);
    snprintf(b, sizeof(b), "%s  →  %s", prVersion(), ota.latest() + (ota.latest()[0] == 'v' ? 1 : 0));
    g.text(r.x + 56, r.y + 44, b, F_ROWB, C_ACC, AL_L, r.w - 70);
    /*  перший рядок опису випуску  */
    char line[96]; const char* s = ota.notes(); size_t n = 0;
    while(s[n] && s[n] != '\n' && n < sizeof(line) - 1){ line[n] = s[n]; n++; }
    line[n] = 0;
    const char* l = line; while(*l == '#' || *l == ' ' || *l == '*' || *l == '-') l++;
    g.text(r.x + 16, r.y + 76, *l ? l : "оновлення з GitHub", F_ROW, C_TXT2, AL_L, r.w - 32);
    g.text(r.x + 16, r.y + 94, "оновитись пізніше: Меню » Оновлення", F_SM, C_TXT3, AL_L, r.w - 32);
  }else{
    g.circle(r.x + 30, r.y + 30, 16, C_RED);
    icon(g, IC_CLOSE, r.x + 30, r.y + 30, 0xFFFF, C_RED);
    g.text(r.x + 56, r.y + 36, "Оновлення не вдалося", F_TITLE, C_TXT, AL_L, r.w - 70);
    g.text(r.x + 16, r.y + 76, ota.error(), F_ROW, C_TXT2, AL_L, r.w - 32);
    g.text(r.x + 16, r.y + 94, "радіо працює на старій версії", F_SM, C_TXT3, AL_L, r.w - 32);
  }
  const int16_t by = r.y + r.h - 46, bw = (r.w - 40) / 2;
  if(_popup == 1){
    g.box(r.x + 14, by, bw, 34, 12, _popBtn == 0 ? C_LINE : C_SURF2);
    g.text(r.x + 14 + bw / 2, by + 22, "Пізніше", F_ROWB, C_TXT, AL_C);
    g.box(r.x + 26 + bw, by, bw, 34, 12, _popBtn == 1 ? C_TXT : C_ACC);
    g.text(r.x + 26 + bw + bw / 2, by + 22, "Оновити", F_ROWB, C_ACCTXT, AL_C);
  }else{
    g.box(r.x + 14, by, r.w - 28, 34, 12, _popBtn >= 0 ? C_LINE : C_SURF2);
    g.text(r.x + r.w / 2, by + 22, "Зрозуміло", F_ROWB, C_TXT, AL_C);
  }
}

void Player::_draw(Gfx& g){
  uint32_t now = _frameT;
  _drawTop(g, now);
  _drawCard(g, now);
  _ripple(g);
  _drawClock(g);
  _drawRow(g);
  _drawVol(g);
  if(_popup) _drawPopup(g);
}

/*  p2perf: куди йде час кадру головного екрана (мкс, з минулого виклику команди)  */
volatile uint32_t p2Frames = 0, p2Strips = 0, p2Px = 0, p2BgUs = 0, p2DrawUs = 0, p2PushUs = 0, p2EndUs = 0, p2MaxUs = 0;
volatile uint32_t p2Calls = 0, p2PreUs = 0, p2SpecUs = 0, p2LoopGap = 0;

void Player::_flush(){
  uint32_t rows[TROWS];
  portENTER_CRITICAL(&_mux);
  memcpy(rows, _dirty, sizeof(rows));
  memset(_dirty, 0, sizeof(_dirty));
  portEXIT_CRITICAL(&_mux);
  bool any = false; uint8_t dirtyRows = 0;
  for(int r = 0; r < TROWS; r++) if(rows[r]){ any = true; dirtyRows++; }
  if(!any) return;
  /*  Повний перемальовок (перехід меню↔плеєр) копіює ~150 КБ тла з PSRAM —
      суцільним потоком це відбирало шину в декодера звуку на іншому ядрі й
      давало провал. Розриваємо його короткими уступками: тло темне (перехід
      іде при згаслій підсвітці), тож зайві мілісекунди непомітні.  */
  const bool bigRedraw = dirtyRows >= 6;
  uint8_t stripN = 0;
  /*  як у меню: половина буфера малюється, поки друга йде шиною  */
  uint16_t* scr = (uint16_t*)lcdScratch((size_t)STRIP * 2);
  if(!scr) return;
  const int32_t HALF = LCD_PUSH_COPIES ? STRIP : STRIP / 2;
  uint16_t* half[2] = { scr, LCD_PUSH_COPIES ? scr : scr + HALF };
  uint8_t hi = 0;
  Gfx g;
  _frameT = millis();                       /* одна мить на весь кадр: смуги виходять по черзі, а хвиля не має рватися */
  const uint32_t uF = micros();
  _rf.on = _ripOn; _rf.rel = _ripRel; _rf.x = _ripX; _rf.y = _ripY; _rf.t0 = _ripT0; _rf.up = _ripUpT;
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
      /*  широкий рядок плиток — шматками по рядках (див. m2ui.cpp)  */
      const int16_t step = lcdRowsFor(w, HALF);
      for(int16_t yy0 = 0; yy0 < h && step > 0; yy0 += step){
        const int16_t hh = (h - yy0 < step) ? (h - yy0) : step;
        const int16_t ys = y0 + yy0;
        uint16_t* buf = half[hi];
        const uint32_t u0 = micros();
        /*  тло — готове, із PSRAM  */
        for(int16_t yy = 0; yy < hh; yy++){
          if(_bg) memcpy(buf + (int32_t)yy * w, _bg + (int32_t)(ys + yy) * PSW + x0, (size_t)w * 2);
          else for(int16_t i = 0; i < w; i++) buf[(int32_t)yy * w + i] = C_BG;
        }
        const uint32_t u1 = micros();
        g.target(buf, x0, ys, w, hh);
        _draw(g);
        const uint32_t u2 = micros();
        if(lcdPush(x0, ys, w, hh, buf)) hi ^= 1;
        p2BgUs += u1 - u0; p2DrawUs += u2 - u1; p2PushUs += micros() - u2; p2Strips++; p2Px += (uint32_t)w * hh;
        if(bigRedraw && (++stripN % 2) == 0){ lcdWait(); vTaskDelay(1); }
      }
    }
  }
  { const uint32_t uE = micros(); lcdEnd(); const uint32_t uX = micros(); p2EndUs += uX - uE; p2Frames++; if(uX - uF > p2MaxUs) p2MaxUs = uX - uF; }
  g_m2Draw = false;
  /*  Кадр вийшов — задача екрана наступний оберт робить без 10-мс сну (як у меню: 2 мс, раз на
      чверть секунди 8 мс). Колись тут кадрів не рахували: головний екран оновлюється постійно
      (спектр), і без сну задача простою не отримувала часу. Тепер вивід на панель чекає гасіння
      розгортки семафором (блокуючись), і простій іде саме тоді. З 10-мс сном екран плеєра давав
      ~23 кадри/с — повзунок гучності й спектр рухались ривками (власник 26.10.2026).  */
  g_m2Frames++;
}

void Player::render(){
  if(!_shown) return;
  uint32_t now = millis();
  const uint32_t uR = micros();
  { static uint32_t prevR = 0; if(prevR && uR - prevR > p2LoopGap) p2LoopGap = uR - prevR; prevR = uR; }
  if(_logoRetryT && now - _logoRetryT > 4000){ _logoRetryT = 0; _logoKey = 0xFFFFFFFF; }   /* ще раз попросити логотип */
  _loadLogo();
  _loadFav();
  if(now - _sigT >= 250){
    _sigT = now;
    /*  шапка  */
    uint32_t s = mixs(2166136261UL, config.station.name);
    s = s * 31 + (player.remoteStationName ? 2 : 0) + config.getMode();
    s = s * 31 + (extras.batMv() >= 2800 && !extras.s.noBat ? extras.batPct() + 1 : 0) + (extras.onPower() ? 500 : 0) + (extras.lowBattery() ? 1000 : 0);
    s = s * 31 + (mic.listening() ? 1 : 0) + extras.s.alarmOn * 2 + extras.sleepLeft() * 4;
    s = s * 31 + (recorder.active() ? recorder.seconds() / 60 + 1 : 0);
    int rs = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -127;
    s = s * 31 + (rs > -55 ? 4 : rs > -65 ? 3 : rs > -75 ? 2 : rs > -85 ? 1 : 0);
    if(s != _sTop){ _sTop = s; _t0Name = now; _mark(0, 0, SW, TOP_H); }
    /*  картка  */
    uint32_t c = mixs(2166136261UL, config.station.title) * 31 + (player.status() == PLAYING) + config.station.bitrate * 7 + (_logoOk ? 3 : 0);
    c = c * 31 + (uint32_t)sermons.playing() + (sermons.coverPix() ? 9 : 0) + sermons.coverVersion() * 131;
    c = c * 31 + (sermonOn() ? 77 : 0) + (sermons.coverBig() ? 5 : 0);
    if(c != _sCard){ _sCard = c; _t0Title = now; _mark(0, CARD_Y, SW, CLK_Y + CLK_H - CARD_Y); }
    /*  годинник  */
    const struct tm& t = network.timeinfo;
    uint32_t k = (uint32_t)t.tm_min * 61 + t.tm_hour * 3600 + t.tm_mday * 99991 + (timekeeper.weatherHave ? (int)lroundf(timekeeper.weatherTemp) * 7 + timekeeper.weatherIcon : 0);
    if(k != _sClock){ _sClock = k; _mark(0, CLK_Y, SW, CLK_H); }
    if((uint32_t)t.tm_sec != _sSec && !sermonOn()){ _sSec = t.tm_sec; _mark(120, CLK_Y + 20, 80, 36); }
    /*  третій рядок  */
    uint8_t m = _mode();
    uint32_t r = m * 1000003UL + (uint32_t)(extras.favPlaying() + 2) * 31 + _favKey;
    if(m == 2) r = r * 31 + player.posSec() + player.durSec() * 7;
    if(r != _sRow || m != _rowMode){ _sRow = r; _rowMode = m; _mark(0, ROW_Y, SW, ROW_H); }
    /*  гучність  */
    if((uint32_t)config.store.volume != _sVol){ _sVol = config.store.volume; _mark(0, VOL_Y, SW, VOL_H); }
  }
  /*  Плавна гучність — за часом, а не за кадрами: показане значення наздоганяє ціль (палець,
      веб, мікрофон) зі сталою часу 40 мс. Було 35 % за кадр: за повільних кадрів повзунок
      відставав від пальця («гальмує»); потім під пальцем рух прибрали зовсім — і пропала
      анімація (власник 26.10.2026). Тепер стрибок через усю смугу доїжджає за ~0,12 с,
      а за пальцем, що веде, повзунок відстає не більше ніж на кадр-два.  */
  {
    const float target = _volDrag >= 0 ? (float)_volDrag : (float)config.store.volume;
    const uint32_t dt = now - _volAnimT;
    _volAnimT = now;
    if(_volShownF < 0) _volShownF = target;
    if(fabsf(target - _volShownF) > 0.5f){
      const float k = 1.0f - expf(-(float)(dt > 100 ? 100 : dt) / 40.0f);
      _volShownF += (target - _volShownF) * k;
      if(fabsf(target - _volShownF) <= 1.5f) _volShownF = target;
      _mark(0, VOL_Y, SW, VOL_H);
#ifdef YO_DEBUG
      if(::yoVolTrace) Serial.printf("VOLANIM %u мс: показано %.1f, ціль %.0f\n", (unsigned)(now % 100000), _volShownF, target);
#endif
    }
  }
  /*  біжучі рядки  */
  if(now - _mqT >= 25){
    _mqT = now;
    int16_t nw = _statusLeft() - 8 - 42;
    int16_t tw = Gfx::textW(config.station.name, F_TITLE);
    if(tw > nw && now - _t0Name > 1500){
      if(++_offName >= tw + 40){ _offName = 0; _t0Name = now; }        /* коло пройшло — знову пауза на початку */
      _mark(42, 0, nw, TOP_H);
    }else if(tw <= nw) _offName = 0;
    if(!sermonOn()){
      char l1[160], l2[160]; cardLines(l1, sizeof(l1), l2, sizeof(l2));
      int16_t w = MX + CWID - 44 - (MX + 76);
      int16_t t1 = Gfx::textW(l1, F_ROWB);
      if(t1 > w && now - _t0Title > 2000){
        if(++_offTitle >= t1 + 40){ _offTitle = 0; _t0Title = now; }
        _mark(MX + 76, CARD_Y + 8, w, 24);
      }else if(t1 <= w) _offTitle = 0;
    }
  }
  /*  рівень звуку й риски в картці  */
  yoSpec.wanted = (_mode() == 0);          /* з картки риски не показуємо — і не рахуємо */
  if(yoSpec.wanted && now - _specT >= 30){
    _specT = now;
    const uint32_t uS = micros();
    bool playing = player.status() == PLAYING;
    uint16_t vu = playing ? player.get_VUlevel(100) : 0;
    (void)vu;
    /*  справжній спектр: що звучить, те й видно; тиша — риски стоять  */
    yoSpec.bands(_spec, 32, playing ? player.getSampleRate() : 0);
    memcpy(::m2SpecDbg, _spec, sizeof(_spec));
    bool moved = false;
    for(uint8_t k = 0; k < 32; k++){
      /*  висота дробова, як і раніше: кінець риски згладжений, рух — плавний, без сходинок у піксель  */
      float h = _spec[k] > 0 ? 3 + _spec[k] * 20 : 0;
      if(fabsf(h - _specH[k]) > 0.2f){ _specH[k] = h; moved = true; }
    }
    if(moved && _mode() == 0) _mark(0, ROW_Y, SW, ROW_H);
    p2SpecUs += micros() - uS;
  }
  /*  риски в картці — ті самі смуги спектра, зведені в п'ять (низи … верхи);
      щойно спектр оновився (раз на 30 мс), а не раз на 90 мс — інакше риски
      відставали від звуку й сіпались  */
  if(_barT != _specT){
    _barT = _specT;
    bool ch = false;
    for(uint8_t k = 0; k < 5; k++){
      float m = 0;
      for(uint8_t j = k * 6; j < k * 6 + 6 && j < 32; j++) if(_spec[j] > m) m = _spec[j];
      if(fabsf(m - _bars[k]) > 0.01f) ch = true;
      _bars[k] = m;
    }
    if(ch && !sermonOn()) _mark(MX + CWID - 44, CARD_Y + 10, 40, CARD_H - 20);
  }
  /*  пропозиція оновитись / повідомлення, що не вдалося  */
  {
    int8_t want = 0;
    const uint32_t beat = extras.lowBatBeat();
    if(beat != _lowSeen){ _lowSeen = beat; _lowUntil = now + 6000; }
    if(_status) want = 3 + _status;                                   /* стан радіо важливіший за решту */
    else if(_lowUntil && (int32_t)(now - _lowUntil) < 0 && extras.lowBattery()) want = 3;
    else if(ota.state() == OTA_ERROR && _otaWasInstalling) want = 2;
    else if(ota.available() && !ota.installing() && strcmp(ota.latest(), _dismiss)) want = 1;
    if(ota.installing()) _otaWasInstalling = true;
    if(want != _popup){ _popup = want; invalAll(); }
    else if(_popup == 4 || _popup == 6){ Rect pr = _popRect(); _mark(pr.x + pr.w / 2 - 20, pr.y + pr.h - 44, 40, 32); }   /* крутилка */
    else if(_popup == 5){ static int32_t shownN = -2; if(_statusN != shownN){ shownN = _statusN; invalAll(); } }
  }
  if(_ripOn){
    if(_ripRel){
      const uint32_t from = _ripUpT > _ripT0 + 200 ? _ripUpT : _ripT0 + 200;
      if(millis() > from + 270) _ripOn = false;
    }
    _mark(MX, CARD_Y, CWID, _cardH());
  }
  static bool wasRip = false;
  if(wasRip && !_ripOn) _mark(MX, CARD_Y, CWID, _cardH());
  wasRip = _ripOn;
  p2PreUs += micros() - uR; p2Calls++;
  _flush();
}

/*  ---------- дотики (головний цикл) ---------- */
void Player::onPress(int16_t x, int16_t y){
  _px = _lx = x; _py = _ly = y; _pt = millis(); _down = true;
  if(_popup){
    /*  картка зверху: лише її кнопки (з притяганням — уся нижня частина картки)  */
    Rect r = _popRect();
    _zone = 20;
    _popBtn = -1;
    if(y >= r.y + r.h - 62 && y < r.y + r.h + 16) _popBtn = (_popup == 1 && x >= SW / 2) ? 1 : 0;
    if(_popup >= 4) _popBtn = -1;                       /* стан радіо — без кнопок */
    _mark(r.x, r.y, r.w, r.h);
    return;
  }
  /*  Гучність ловимо з запасом: у ряду рівня звуку дотик нічого не робить, тож
      увесь низ від нього — повзунок; інакше — від середини проміжку над смугою.  */
  bool specRow = _mode() == 0;
  /*  Кнопки притягують дотик: шапка ловить на 16 пікселів нижче себе, значок
      джерела й меню — за відстанню до центру (до 36 пікселів), а пауза в
      картці спрацьовує лише всередині неї, не на краю під шапкою.  */
  auto near = [&](int16_t cx, int16_t cy){ int32_t dx = x - cx, dy = y - cy; return dx * dx + dy * dy <= 36 * 36; };
  if(y < TOP_H + 16){
    if(near(20, 19)) _zone = 0;
    else if(near(SW - 20, 19)) _zone = 2;
    else _zone = 1;
  }
  /*  Значок станції в картці — перелік станцій, одразу на тій, що грає.
      Решта картки лишається паузою: її тиснуть найчастіше.  */
  else if(!sermonOn() && x >= MX + 8 && x < MX + 8 + LOGO && y >= CARD_Y + 8 && y < CARD_Y + 8 + LOGO){
    _zone = 7;
    _ripX = x; _ripY = y; _ripT0 = _pt; _ripUpT = 0; _ripRel = false; _ripOn = true;
  }
  else if(y >= CARD_Y + 14 && y < CARD_Y + _cardH()){
    _zone = 3;
    _ripX = x; _ripY = y; _ripT0 = _pt; _ripUpT = 0; _ripRel = false; _ripOn = true;
  }
  else if(y < CARD_Y + _cardH()) _zone = 6;
  else if(specRow && y >= ROW_Y - 6) _zone = 5;
  else if(y >= VOL_Y - 5) _zone = 5;
  else if(y >= ROW_Y - 6) _zone = 4;
  else _zone = 6;
  if(_zone == 5){ onDrag(x, y); }
  if(_zone == 4 && _mode() == 2){
    /*  кнопки ⏮ ⏭ ловлять ширше за себе: до 60 пікселів від краю  */
    if(x < 60){ _btn = 0; _mark(0, ROW_Y, 70, ROW_H); }
    else if(x >= SW - 60){ _btn = 1; _mark(SW - 70, ROW_Y, 70, ROW_H); }
    else onDrag(x, y);
  }
}

void Player::onDrag(int16_t x, int16_t y){
  _lx = x; _ly = y;
  /*  повели пальцем (до списку станцій) — хвиля гасне  */
  if(_ripOn && !_ripRel && (abs(x - _px) > 14 || abs(y - _py) > 14)){
    if(millis() - _ripT0 < 90) _ripOn = false;      /* хвиля ще не почалась — жест, а не дотик: без неї */
    else { _ripUpT = millis(); _ripRel = true; }
  }
  if(_zone == 20) return;
  if(_zone == 5){
    /*  доріжка 49..255; краї липкі — біля кінця одразу мінімум чи максимум  */
    int v;
    if(x <= 58) v = 0;
    else if(x >= SW - 66) v = 254;
    else v = map((int)x, 58, SW - 66, 0, 254);
    if(v < 0) v = 0; if(v > 254) v = 254;
    _volDrag = v;
    _mark(0, VOL_Y, SW, VOL_H);
    uint32_t now = millis();
    /*  у чергу плеєра не частіше ніж раз на 120 мс; кінцеве — на відпусканні  */
    if(v != (int)config.store.volume && now - _volSent >= 120){ _volLast = v; _volSent = now; player.setVol((uint8_t)v); }
    return;
  }
  if(_zone == 4 && _mode() == 2 && _btn < 0){
    float f = (x - 76) / (float)(SW - 152);
    if(f < 0) f = 0; if(f > 1) f = 1;
    _seek = f; _mark(0, ROW_Y, SW, ROW_H);
  }
}

void Player::onRelease(int16_t x, int16_t y){
  (void)x; (void)y;
  if(_ripOn && !_ripRel){ _ripUpT = millis(); _ripRel = true; }
  int8_t z = _zone;
  if(z == 20){
    int8_t b = _popBtn;
    _down = false; _zone = -1; _popBtn = -1;
    if(b >= 0 && _popup == 1){
      if(b == 1) ota.install();
      else strlcpy(_dismiss, ota.latest(), sizeof(_dismiss));
    }else if(b >= 0 && _popup == 2){
      _otaWasInstalling = false;
    }else if(b >= 0 && _popup == 3){
      _lowUntil = 0;
    }
    invalAll();
    return;
  }
  _down = false; _upT = millis();
  _zone = -1;
  int16_t dx = _lx - _px, dy = _ly - _py;
  bool tap = abs(dx) < 14 && abs(dy) < 14;
  if(z == 5){
    /*  порівнюємо з тим, що є насправді: гучність могли змінити й з веба  */
    if(_volDrag >= 0 && _volDrag != (int)config.store.volume){ _volLast = _volDrag; player.setVol((uint8_t)_volDrag); }
    int16_t v = _volDrag; (void)v;
    _volDrag = -1; _mark(0, VOL_Y, SW, VOL_H);
    return;
  }
  if(z == 4 && _mode() == 2){
    bool sm = sermonOn();
    if(_btn == 0){ if(sm) sermons.playRel(-1); else player.prev(); }
    else if(_btn == 1){ if(sm) sermons.playRel(1); else player.next(); }
    else if(_seek >= 0){
      uint32_t dur = player.durSec();
      /*  Файл картки — через чергу плеєра (Player::seekTo): перемотка мусить статись у головному
          циклі між кроками декодування. Раніше setAudioPlayPosition кликали просто з задачі
          екрана — посеред декодування, звідси сміття й клацання на перемотці.  */
      if(dur){
        if(sm) player.burlSeek((uint32_t)(_seek * dur));
        else if(player.sd_max > player.sd_min) player.seekTo(player.sd_min + (uint32_t)((uint64_t)(player.sd_max - player.sd_min) * _seek));
        else player.setAudioPlayPosition((uint16_t)(_seek * dur));
      }
    }
    _btn = -1; _seek = -1; _mark(0, ROW_Y, SW, ROW_H);
    return;
  }
  /*  провели пальцем угору чи вниз — список станцій  */
  if(abs(dy) > 30 && abs(dy) > abs(dx) && (z == 1 || z == 3 || z == 6 || z == 7)){
    display.putRequest(NEWMODE, STATIONS);
    return;
  }
  if(!tap) return;
  /*  довгий дотик по годиннику чи картці — список станцій, як і раніше  */
  if((z == 3 || z == 6) && millis() - _pt > 700){ display.putRequest(NEWMODE, STATIONS); return; }
  switch(z){
    case 0:
      /*  У проповіді тут стрілка «назад»: проповідь стає (місце запам'ятовується), грає
          радіо — остання станція. Раніше той самий дотик перемикав на картку пам'яті:
          проповідь лишалась на екрані, а «Станції» в меню відкривали картку.  */
      if(player.remoteStationName && !dlna.playing() && !player.extOn){ player.sendCommand({PR_PLAY, config.lastStation()}); break; }
      if(!extras.s.noSd) config.changeMode();
      break;
    case 1: display.putRequest(NEWMODE, STATIONS); break;
    case 2: yomenu.openHome(); break;
    case 3: player.toggle(); _mark(MX, CARD_Y, CWID, _cardH()); break;
    case 4:
      if(_mode() == 1){
        /*  найближчий кружок по горизонталі (центри через 52 пікселі від 30)  */
        int i = ((int)_px - 30 + 26) / 52; if(i < 0) i = 0; if(i > 5) i = 5;
        if(extras.fav[i].url[0]) extras.favPlay((uint8_t)i);
        else yomenu.openFav();
      }
      break;
    case 6: break;                        /* годинник і край картки — нічого: випадкова зупинка гірша за зайвий дотик */
    case 7: display.putRequest(NEWMODE, STATIONS); break;   /* значок станції */
  }
}

}  // namespace m2
