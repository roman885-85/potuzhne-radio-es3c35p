/*  Новий список станцій (і треків картки пам'яті) — сторінка нового меню.

    Замість старого жовто-чорного списку з кнопками праворуч: одна картка
    з рядками — логотип станції (або літери назви), назва, номер і адреса.
    Та, що вибрана, — жовта, а поки грає, праворуч живі риски спектра.
    Прокрутка пальцем з інерцією, як у решті меню; довгий список (картка
    пам'яті) можна гнати за правий край — з'являється номер рядка.

    Назви й логотипи читає окрема задача: спершу видимі, далі решту підряд.
    У головному циклі читати не можна — там опитується дотик, і поки файл
    відкривається, список ішов за пальцем ривками. Задача дисплея малює лише
    те, що вже прочитано (решта — сірі заготовки). Прокрутка доводиться
    пружиною рівно до рядка, як у старому списку.  */
#include "../core/options.h"
#include "m2pages.h"
#include <SPIFFS.h>
#include "../core/config.h"
#include "../core/player.h"
#include "../extras/yoExtras.h"
#include "../extras/yoLogos.h"
#include "../extras/yoSpectrum.h"
#include "esp_heap_caps.h"

namespace m2 {

static const int16_t  RH = 48;          /* рядок: 4 рядки якраз на екран */
static const int16_t  TOP = 4;          /* поле над карткою */
static const int16_t  BS = 34;          /* логотип */
static const int16_t  FW = 16;          /* смуга швидкої прокрутки праворуч */
/*  Координати вмісту сторінки — 16-бітні: 48 × 660 ≈ 31 700 пікселів. Більше
    рядків список не показує (гра далі/назад по картці працює як і раніше).  */
static const uint16_t MAXROWS = 660;

struct StRow { char name[88]; char sub[48]; uint32_t crc; };

static volatile bool s_req = false;
/*  Кеш готових рядків (команда m2cache). На ES3C35P (масштаб 4/3) рядок у кеші —
    у пікселях панелі, а Gfx зсуває вміст сторінки цілими пікселями панелі
    (Gfx::origin), тож готовий рядок лягає точно туди ж, куди ліг би
    намальований наново. Під час прокрутки кадр лише копіює рядки.  */
volatile bool m2RowCache = true;

class StationsPage : public Page {
  public:
    const char* title() override { return _sd ? "Картка пам'яті" : "Радіостанції"; }
    void enter() override;
    void leave() override { _active = false; }
    void tick(uint32_t now) override;
    int16_t snapStep() override { return RH; }
    int16_t height() override { return _n ? TOP + _n * RH + 10 : CH; }
    void draw(Gfx& g) override;
    int16_t hit(int16_t x, int16_t y, Rect& r, uint8_t& radius) override;
    uint8_t grab(int16_t id) override { return id == FAST ? 2 : 0; }
    void drag(int16_t id, int16_t x, int16_t y, bool end) override;
    void tap(int16_t id, int16_t x, int16_t y) override;
    bool hold(int16_t id) override;
  private:
    static const int16_t FAST = 30000;
    volatile uint16_t _n = 0;
    uint16_t _cap = 0;
    bool _sd = false;
    StRow*     _rows = nullptr;         /* PSRAM */
    uint8_t*   _st = nullptr;           /* 1 — рядок прочитано */
    uint16_t** _logo = nullptr;         /* логотипи 34×34 (PSRAM), з'являються, коли треба */
    uint8_t*   _lst = nullptr;          /* 0 — не шукали, 1 — є, 2 — немає */
    uint16_t _seq = 1;                  /* решту списку читаємо підряд звідси */
    uint32_t _seqT = 0;
    volatile uint32_t _ver = 0;
    uint32_t _shownVer = 0, _sig = 0, _sigT = 0, _barT = 0, _fastT = 0;
    bool     _fastOn = false;
    float    _bars[3] = { 0, 0, 0 };
    uint32_t _favCrc[FAV_N] = { 0 };
    volatile bool _active = false;
    SemaphoreHandle_t _lock = nullptr;  /* задача читання ↔ перевиділення пам'яті в enter() */
    char _line[420];
    static void _taskFn(void* arg){ ((StationsPage*)arg)->_run(); }
    void _run();
    bool _step();                       /* одна порція роботи; false — робити нічого */
    void _read(uint16_t from, uint8_t count);
    void _loadLogo(uint16_t i);
    /*  Намальований рядок (без рисок спектра) — у пам'яті: під час прокрутки кадр
        лише копіює готові рядки, а не малює наново літери й логотипи.  */
    struct Slot { int32_t idx = -1; uint32_t sig = 0; uint32_t used = 0; uint16_t* px = nullptr; };   /* px — пікселі панелі */
    static const uint8_t SLOTS = 10;
    Slot _slot[SLOTS];
    uint32_t _useN = 0, _gen = 0;
    void _drawRow(Gfx& g, int32_t i, int16_t y, bool on, bool playing, int16_t cur);
    const uint16_t* _cached(int32_t i, int16_t y, bool on, bool playing, int16_t cur);
    int16_t _cur() const { int16_t c = (int16_t)config.lastStation() - 1; return c >= 0 && c < (int16_t)_n ? c : -1; }
    bool _playing() const { return player.status() == PLAYING && !player.remoteStationName; }
    int16_t _thumbH(){ int16_t h = height(); int16_t t = (int16_t)((int32_t)CH * CH / h); return t < 28 ? 28 : t; }
};

/*  ---------- читання списку (своя задача; перші рядки — в enter()) ---------- */

/*  «http://online.hitfm.ua/HitFM_Top» → «online.hitfm.ua»  */
static void hostOf(const char* url, char* out, size_t cap){
  const char* s = strstr(url, "://");
  s = s ? s + 3 : url;
  if(!strncmp(s, "www.", 4)) s += 4;
  size_t k = 0;
  while(s[k] && s[k] != '/' && s[k] != ':' && s[k] != '?' && k + 1 < cap){ out[k] = s[k]; k++; }
  out[k] = 0;
}

void StationsPage::_read(uint16_t from, uint8_t count){
  if(from < 1 || from > _n) return;
  FS* fs = config.SDPLFS();
  File index = fs->open(REAL_INDEX, "r");
  if(!index) return;
  uint32_t pos = 0;
  index.seek((from - 1) * 4, SeekSet);
  bool ok = index.readBytes((char*)&pos, 4) == 4;
  index.close();
  if(!ok) return;
  File pl = fs->open(REAL_PLAYL, "r");
  if(!pl) return;
  pl.seek(pos, SeekSet);
  char* line = _line;
  for(uint8_t k = 0; k < count && from + k <= _n; k++){
    if(!pl.available()) break;
    size_t len = pl.readBytesUntil('\n', line, sizeof(_line) - 1);
    if(len == sizeof(_line) - 1){ int c; while((c = pl.read()) >= 0 && c != '\n'){} }   /* хвіст задовгого рядка */
    line[len] = 0;
    if(len && line[len - 1] == '\r') line[--len] = 0;
    const uint16_t i = from + k - 1;
    if(_st[i]) continue;
    char* url = nullptr;
    char* t1 = strchr(line, '\t');
    if(t1){ *t1 = 0; url = t1 + 1; char* t2 = strchr(url, '\t'); if(t2) *t2 = 0; }
    StRow& r = _rows[i];
    char where[40] = { 0 };
    if(_sd){
      /*  файл: назва без розширення, під нею — тека  */
      strlcpy(r.name, line, sizeof(r.name));
      char* dot = strrchr(r.name, '.');
      char ext[8] = { 0 };
      if(dot && strlen(dot) <= 5){ strlcpy(ext, dot + 1, sizeof(ext)); *dot = 0; }
      if(url){
        const char* slash = strrchr(url, '/');
        if(slash && slash != url){
          const char* p = slash - 1;
          while(p > url && *p != '/') p--;
          if(*p == '/') p++;
          size_t n = slash - p; if(n >= sizeof(where)) n = sizeof(where) - 1;
          memcpy(where, p, n); where[n] = 0;
        }
      }
      if(where[0] && ext[0]) snprintf(r.sub, sizeof(r.sub), "%u · %s · %s", (unsigned)(i + 1), where, ext);
      else if(ext[0])        snprintf(r.sub, sizeof(r.sub), "%u · %s", (unsigned)(i + 1), ext);
      else                   snprintf(r.sub, sizeof(r.sub), "%u", (unsigned)(i + 1));
      r.crc = 0;
    }else{
      strlcpy(r.name, line, sizeof(r.name));
      if(url) hostOf(url, where, sizeof(where));
      if(where[0]) snprintf(r.sub, sizeof(r.sub), "%u · %s", (unsigned)(i + 1), where);
      else snprintf(r.sub, sizeof(r.sub), "%u", (unsigned)(i + 1));
      r.crc = url && url[0] ? YoLogos::crc(url) : 0;
    }
    _st[i] = 1;                        /* останнім: задача дисплея бере рядок лише після цього */
  }
  pl.close();
  _ver++;
}

void StationsPage::_loadLogo(uint16_t i){
  _lst[i] = 2;
  if(_sd || !_rows[i].crc) return;
  char path[28]; snprintf(path, sizeof(path), "/logo/%08x.565", (unsigned)_rows[i].crc);
  if(!SPIFFS.exists(path)) return;
  static uint16_t* raw = (uint16_t*)heap_caps_malloc(LOGO_S * LOGO_S * 2, MALLOC_CAP_SPIRAM);
  if(!raw) return;
  File f = SPIFFS.open(path, "r");
  bool ok = f && f.size() == LOGO_S * LOGO_S * 2 && f.read((uint8_t*)raw, LOGO_S * LOGO_S * 2) == LOGO_S * LOGO_S * 2;
  if(f) f.close();
  if(!ok) return;
  if(!_logo[i]) _logo[i] = (uint16_t*)heap_caps_malloc(BS * BS * 2, MALLOC_CAP_SPIRAM);
  uint16_t* d = _logo[i];
  if(!d) return;
  /*  45 → 34 білінійно  */
  const float k = LOGO_S / (float)BS;
  for(int16_t y = 0; y < BS; y++){
    float fy = (y + 0.5f) * k - 0.5f; if(fy < 0) fy = 0;
    int y0 = (int)fy; int y1 = y0 + 1 < LOGO_S ? y0 + 1 : y0; float ay = fy - y0;
    for(int16_t x = 0; x < BS; x++){
      float fx = (x + 0.5f) * k - 0.5f; if(fx < 0) fx = 0;
      int x0 = (int)fx; int x1 = x0 + 1 < LOGO_S ? x0 + 1 : x0; float ax = fx - x0;
      uint16_t p00 = raw[y0 * LOGO_S + x0], p01 = raw[y0 * LOGO_S + x1], p10 = raw[y1 * LOGO_S + x0], p11 = raw[y1 * LOGO_S + x1];
      auto ch = [&](uint8_t sh, uint8_t m){
        float a = ((p00 >> sh) & m) * (1 - ax) + ((p01 >> sh) & m) * ax;
        float b = ((p10 >> sh) & m) * (1 - ax) + ((p11 >> sh) & m) * ax;
        return (uint16_t)lroundf(a * (1 - ay) + b * ay);
      };
      d[y * BS + x] = (ch(11, 31) << 11) | (ch(5, 63) << 5) | ch(0, 31);
    }
  }
  _lst[i] = 1;
  _ver++;
}

void StationsPage::enter(){
  if(!_lock) _lock = xSemaphoreCreateMutex();
  if(!_lock) return;
  xSemaphoreTake(_lock, portMAX_DELAY);
  _active = false;
  _sd = config.getMode() == PM_SDCARD;
  uint16_t n = config.playlistLength();
  if(n > MAXROWS) n = MAXROWS;
  _n = 0;                              /* поки міняємо пам'ять, малювати нічого */
  if(n > _cap){
    vTaskDelay(pdMS_TO_TICKS(40));     /* кадр, що саме малюється, закінчиться */
    if(_logo) for(uint16_t i = 0; i < _cap; i++) if(_logo[i]) free(_logo[i]);
    free(_rows); free(_st); free(_logo); free(_lst);
    uint16_t cap = n < 64 ? 64 : n;
    _rows = (StRow*)heap_caps_malloc(sizeof(StRow) * cap, MALLOC_CAP_SPIRAM);
    _st   = (uint8_t*)heap_caps_calloc(cap, 1, MALLOC_CAP_SPIRAM);
    _logo = (uint16_t**)heap_caps_calloc(cap, sizeof(uint16_t*), MALLOC_CAP_SPIRAM);
    _lst  = (uint8_t*)heap_caps_calloc(cap, 1, MALLOC_CAP_SPIRAM);
    if(!_rows || !_st || !_logo || !_lst){
      free(_rows); free(_st); free(_logo); free(_lst);
      _rows = nullptr; _st = nullptr; _logo = nullptr; _lst = nullptr; _cap = 0;
      xSemaphoreGive(_lock);
      return;
    }
    _cap = cap;
  }
  if(!_cap){ xSemaphoreGive(_lock); return; }
  memset(_st, 0, _cap); memset(_lst, 0, _cap);
  _seq = 1; _fastOn = false; _sig = 0; _gen++;
  _n = n;
  int16_t cur = _cur();
  if(cur >= 0){
    /*  одразу до тієї, що вибрана: другим рядком, рівно по сітці рядків  */
    int16_t s = (cur - 1) * RH;
    int16_t mx = height() - CH;
    if(s > mx) s = mx;
    if(s < 0) s = 0;
    scroll = s;
  }
  /*  перші видимі — одразу, щоб сторінка в'їхала вже з назвами (один файл, ~12 рядків)  */
  if(_n) _read((cur > 1 ? cur - 1 : 0) + 1, 8);
  _active = true;
  xSemaphoreGive(_lock);
  static bool started = false;
  if(!started) started = xTaskCreatePinnedToCore(_taskFn, "stlist", 5120, this, 1, nullptr, 0) == pdPASS;
}

bool StationsPage::_step(){
  if(!_active || !_n) return false;
  /*  з'явився новий логотип (пошук по всіх станціях іде у тлі) — рядки «нема» перевірити ще раз  */
  static uint32_t lv = 0;
  if(logos.version() != lv){ lv = logos.version(); for(uint16_t i = 0; i < _n; i++) if(_lst[i] == 2) _lst[i] = 0; }
  int16_t sc = scroll;
  int32_t first = (sc - TOP) / RH - 4; if(first < 0) first = 0;
  int32_t last = first + CH / RH + 8; if(last > (int32_t)_n - 1) last = _n - 1;
  for(int32_t i = first; i <= last; i++) if(!_st[i]){ _read(i + 1, 16); return true; }
  if(!_sd) for(int32_t i = first; i <= last; i++) if(_st[i] && !_lst[i]){ _loadLogo(i); return true; }
  /*  решта — підряд: прокрутка далі покаже вже назви  */
  while(_seq <= _n && _st[_seq - 1]) _seq++;
  if(_seq <= _n){ _read(_seq, 24); return true; }
  return false;
}

void StationsPage::_run(){
  for(;;){
    bool did = false;
    if(_lock && xSemaphoreTake(_lock, pdMS_TO_TICKS(50)) == pdTRUE){
      did = _step();
      xSemaphoreGive(_lock);
    }
    /*  між порціями — пауза: на цьому ядрі ще й екран, і мікрофон  */
    vTaskDelay(pdMS_TO_TICKS(did ? 4 : 80));
  }
}

/*  ---------- показ (задача дисплея) ---------- */

void StationsPage::tick(uint32_t now){
  const int16_t sc = scroll;
  if(_ver != _shownVer){ _shownVer = _ver; M.inval(Rect(0, sc, SW, CH)); }
  if(now - _sigT >= 300){
    _sigT = now;
    uint32_t s = (uint32_t)config.lastStation() * 7 + (_playing() ? 1 : 0) + (uint32_t)config.getMode() * 1000;
    for(uint8_t k = 0; k < FAV_N; k++){
      _favCrc[k] = extras.fav[k].url[0] ? YoLogos::crc(extras.fav[k].url) : 0;
      s = s * 31 + _favCrc[k];
    }
    if(s != _sig){ _sig = s; M.inval(Rect(0, sc, SW, CH)); }
  }
  /*  риски навпроти тієї, що грає, — справжній спектр, як на головному  */
  int16_t cur = _cur();
  if(cur >= 0 && _playing() && now - _barT >= 60){
    _barT = now;
    float sp[32];
    yoSpec.bands(sp, 32, player.getSampleRate());
    static const uint8_t G[4] = { 0, 8, 19, 32 };
    bool ch = false;
    for(uint8_t k = 0; k < 3; k++){
      float m = 0;
      for(uint8_t j = G[k]; j < G[k + 1]; j++) if(sp[j] > m) m = sp[j];
      if(fabsf(m - _bars[k]) > 0.03f){ _bars[k] = m; ch = true; }
    }
    if(ch) M.inval(Rect(MX + CWID - 34, TOP + cur * RH, 30, RH));
  }
  if(_fastOn && now - _fastT > 800){ _fastOn = false; M.inval(Rect(0, sc, SW, CH)); }
}

void StationsPage::_drawRow(Gfx& g, int32_t i, int16_t y, bool on, bool playing, int16_t cur){
  const uint16_t n = _n;
  drawCard(g, MX, y, CWID, RH, i == 0, i == (int32_t)n - 1, on ? Gfx::blend(C_SURF, C_ACC, 30) : C_SURF);
  if(on) g.box(MX + 8, y + (RH - BS) / 2 - 2, BS + 4, BS + 4, 10, C_ACC);
  if(i && !on && i - 1 != cur) g.fill(MX + 56, y, CWID - 56, 1, C_LINE);
  const int16_t bx = MX + 10, by = y + (RH - BS) / 2;
  if(!_st || !_st[i]){
    /*  ще не прочитано — заготовка  */
    g.box(bx, by, BS, BS, 8, C_SURF2);
    g.box(MX + 56, y + 12, 130, 10, 5, C_SURF2);
    g.box(MX + 56, y + 30, 70, 7, 3, C_SURF2);
    return;
  }
  const StRow& r = _rows[i];
  if(_sd){
    g.box(bx, by, BS, BS, 8, on ? C_ACC : C_SURF2);
    icon(g, IC_NOTE, bx + BS / 2, by + BS / 2, on ? C_ACCTXT : C_TXT2, on ? C_ACC : C_SURF2);
  }else if(_lst[i] == 1 && _logo[i]){
    g.image(bx, by, BS, BS, _logo[i], 8);
  }else{
    static const uint16_t PAL[8] = { 0x3A8D, 0x5A4B, 0x2C6A, 0x6A28, 0x2B0F, 0x7A6C, 0x4B09, 0x31CC };
    g.box(bx, by, BS, BS, 8, PAL[r.crc & 7]);
    /*  дві перші літери назви (UTF-8)  */
    char ini[8] = { 0 };
    const char* s = r.name; uint8_t k = 0, chars = 0;
    while(*s && chars < 2 && k < 6){
      uint8_t b = (uint8_t)*s;
      uint8_t len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 1;
      if(b == ' ' || b == '"' || b == '\''){ s++; continue; }
      for(uint8_t j = 0; j < len && s[j]; j++) ini[k++] = s[j];
      s += len; chars++;
    }
    g.text(bx + BS / 2, by + 22, ini, F_ROWB, 0xFFFF, AL_C);
  }
  /*  праворуч: риски (грає) і зірка (в обраному)  */
  int16_t right = MX + CWID - 10;
  if(on && playing) right -= 28;             /* місце під риски — їх малює draw() поверх */
  bool fav = false;
  if(r.crc) for(uint8_t k = 0; k < FAV_N; k++) if(_favCrc[k] == r.crc) fav = true;
  if(fav){ icon(g, IC_STAR, right - 9, y + RH / 2, on ? C_ACC : C_TXT3, on ? Gfx::blend(C_SURF, C_ACC, 30) : C_SURF); right -= 24; }
  const int16_t tx = MX + 56, tw = right - tx - 4;
  g.text(tx, y + 21, r.name, on ? F_ROWB : F_ROW, on ? C_ACC : C_TXT, AL_L, tw);
  g.text(tx, y + 38, r.sub, F_SM, C_TXT2, AL_L, tw);
}

const uint16_t* StationsPage::_cached(int32_t i, int16_t y, bool on, bool playing, int16_t cur){
  const StRow& r = _rows[i];
  bool fav = false;
  if(r.crc) for(uint8_t k = 0; k < FAV_N; k++) if(_favCrc[k] == r.crc) fav = true;
  uint32_t sig = _gen * 2654435761UL ^ r.crc;
  sig = sig * 31 + (on ? 1 : 0) + (on && playing ? 2 : 0) + (fav ? 4 : 0) + (_lst[i] * 8) + (i - 1 == cur ? 64 : 0) + (_sd ? 128 : 0);
  /*  розмір рядка в пікселях панелі (на 4/3 залежить від того, як ліг y)  */
  const int16_t pw = uiP(MX + CWID) - uiP(MX), ph = uiP(y + RH) - uiP(y);
  sig = sig * 31 + (uint32_t)ph;
  Slot* slot = nullptr;
  for(uint8_t k = 0; k < SLOTS; k++){
    Slot& sl = _slot[k];
    if(sl.idx == i && sl.px){
      if(sl.sig == sig){ sl.used = ++_useN; return sl.px; }
      slot = &sl; break;                        /* той самий рядок змінився — малюємо на його місці */
    }
  }
  if(!slot){
    slot = &_slot[0];
    for(uint8_t k = 0; k < SLOTS; k++){
      if(!_slot[k].px || _slot[k].idx < 0){ slot = &_slot[k]; break; }
      if(_slot[k].used < slot->used) slot = &_slot[k];
    }
  }
  if(!slot->px) slot->px = (uint16_t*)heap_caps_malloc((size_t)(uiP(CWID) + 2) * (uiP(RH) + 2) * 2, MALLOC_CAP_SPIRAM);
  if(!slot->px) return nullptr;
  Gfx rg;
  rg.target(slot->px, 0, 0, pw, ph);
  rg.origin(-MX, -y);                           /* (MX, y) → піксель панелі 0,0 */
  _drawRow(rg, i, y, on, playing, cur);
  slot->idx = i; slot->sig = sig; slot->used = ++_useN;
  return slot->px;
}

void StationsPage::draw(Gfx& g){
  const uint16_t n = _n;
  if(!n){
    g.box(MX, 24, CWID, 130, R_CARD, C_SURF);
    icon(g, _sd ? IC_CARD : IC_RADIO, SW / 2, 60, C_ACC, C_SURF);
    g.text(SW / 2, 100, _sd ? "на картці немає музики" : "список станцій порожній", F_ROW, C_TXT, AL_C, CWID - 20);
    g.text(SW / 2, 124, _sd ? "mp3, m4a, aac, flac, wav — у будь-якій теці" : "додайте станції на сторінці радіо в браузері", F_SM, C_TXT2, AL_C, CWID - 20);
    return;
  }
  const int16_t sc = scroll;
  const int16_t cur = _cur();
  const bool playing = _playing();
  int32_t first = (sc - TOP) / RH - 1; if(first < 0) first = 0;
  for(int32_t i = first; i < (int32_t)n && i < first + CH / RH + 3; i++){
    const int16_t y = TOP + i * RH;
    if(!g.visible(MX, y, CWID, RH)) continue;
    const bool on = i == cur;
    /*  крайні рядки мають заокруглені кути над тлом із переходом — їх малюємо щоразу  */
    const uint16_t* px = (m2RowCache && i > 0 && i < (int32_t)n - 1 && _st && _st[i]) ? _cached(i, y, on, playing, cur) : nullptr;
    if(px) g.blit(MX, y, uiP(MX + CWID) - uiP(MX), uiP(y + RH) - uiP(y), px);
    else _drawRow(g, i, y, on, playing, cur);
    if(on && playing && _st && _st[i]){
      const int16_t right = MX + CWID - 10;
      for(uint8_t k = 0; k < 3; k++){
        const float h = 4 + _bars[k] * 18;
        const float xx = right - 18 + k * 7;
        g.line(xx, y + 33, xx, y + 33 - h, 3.2f, C_ACC);
      }
    }
  }
  /*  повзунок прокрутки; під час швидкої — номер рядка поруч  */
  const int16_t H = height();
  if(H > CH){
    int16_t s = sc < 0 ? 0 : (sc > H - CH ? H - CH : sc);
    const int16_t th = _thumbH();
    const int16_t ty = s + (int16_t)((int32_t)(CH - th) * s / (H - CH));
    g.box(SW - 6, ty + 3, 3, th - 6, 2, _fastOn ? C_ACC : C_TXT3);
    if(_fastOn){
      int32_t idx = (s + CH / 2 - TOP) / RH + 1;
      if(idx < 1) idx = 1;
      if(idx > n) idx = n;
      char b[8]; snprintf(b, sizeof(b), "%ld", (long)idx);
      int16_t by = ty + th / 2 - 17;
      if(by < s + 4) by = s + 4;
      if(by > s + CH - 38) by = s + CH - 38;
      g.box(SW - 78, by, 62, 34, 12, C_ACC);
      g.text(SW - 47, by + 24, b, F_MID, C_ACCTXT, AL_C);
    }
  }
}

int16_t StationsPage::hit(int16_t x, int16_t y, Rect& r, uint8_t& radius){
  if(!_n) return -1;
  if(height() > CH * 3 && x >= SW - FW){ r = Rect(SW - FW, scroll, FW, CH); radius = 0; return FAST; }
  int16_t i = (y - TOP) / RH;
  if(y < TOP || i < 0 || i >= (int16_t)_n) return -1;
  r = Rect(MX, TOP + i * RH, CWID, RH);
  radius = (i == 0 || i == (int16_t)_n - 1) ? R_CARD : 0;
  return i;
}

void StationsPage::drag(int16_t id, int16_t x, int16_t y, bool end){
  (void)x; (void)end;
  if(id != FAST) return;
  const int16_t H = height();
  if(H <= CH) return;
  const int16_t th = _thumbH();
  float f = ((y - scroll) - th / 2) / (float)(CH - th);     /* палець на екрані, у межах вмісту */
  if(f < 0) f = 0;
  if(f > 1) f = 1;
  const int16_t s = (int16_t)lroundf(f * (H - CH));
  _fastOn = true; _fastT = millis();
  if(s != scroll){ scroll = s; M.invalScreen(0, HDR - 1, SW, CH + 1); }
  else M.inval(Rect(SW - 80, scroll, 80, CH));
}

void StationsPage::tap(int16_t id, int16_t x, int16_t y){
  (void)x; (void)y;
  if(id < 0 || id >= (int16_t)_n) return;
  /*  та, що вже грає, — просто назад на головний  */
  if(!(id == _cur() && _playing())) player.sendCommand({PR_PLAY, (int)(id + 1)});
  M.close();
}

bool StationsPage::hold(int16_t id){
  /*  довгий дотик — повна назва (у рядку вона могла не вміститися)  */
  if(id < 0 || id >= (int16_t)_n || !_st[id]) return false;
  M.toast(_rows[id].name);
  return true;
}

static StationsPage s_stations;
Page& pgStations = s_stations;

/*  Хтось попросив список станцій (дотик на плеєрі, кнопка, команда) —
    відкриємо в головному циклі.  */
void stationsRequest(){ s_req = true; }

void stationsPoll(){
  if(!s_req) return;
  s_req = false;
  if(M.active()){ if(M.ltop() != &pgStations) M.push(&pgStations); }
  else M.open(&pgStations);
}

}  // namespace m2
