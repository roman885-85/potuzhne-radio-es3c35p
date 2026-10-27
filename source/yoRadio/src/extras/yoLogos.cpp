#include "yoLogos.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include "miniz.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "yoSermons.h"          /* yoJpegFit */
#include "../core/options.h"
#include "../core/config.h"      /* PLAYLIST_PATH */
#include "../core/player.h"      /* запас у буфері потоку */

YoLogos logos;

/*  Список «нема логотипа» (див. noLoad нижче): у PSRAM і одним файлом.  */
#define LG_NOLIST "/logo/no.lst"
/*  Правила відбору логотипів. 2 (27.10.2026) — перевірка, чи знак прочитається в клітинці,
    темна підкладка під світлий знак, домашня сторінка з каталогу. Список зі старшими
    правилами — один раз переглянути збережене (logoReview).  */
#define LG_RULES  2
static uint32_t* s_no = nullptr;
static uint16_t  s_noN = 0, s_noCap = 0;
static volatile bool s_noLoaded = false;
static bool      s_noDirty = false, s_passDone = false, s_passDone0 = false;
static bool noHas(uint32_t c);
static void noAdd(uint32_t c);
static void noLoad();
static void noSave();
static void waitStream();

bool yoStbDecode(const uint8_t* d, size_t n, uint8_t** rgba, uint32_t* W, uint32_t* H);   /* yoStbImage.cpp */

/*  Найбільший значок, який качаємо. Бувають і PNG 1920×1920 на 1 МБ (slukhay.com —
    «Люкс FM»): такі розбираються потоком і одразу зменшуються (pngFitBig).  */
#define LG_MAXDL   (1536 * 1024)
#define LG_MAXPX   (1024 * 1024)         /* найбільше зображення в пікселях */
#define LG_API     "http://de1.api.radio-browser.info/json/stations/"

uint32_t YoLogos::crc(const char* s){
  uint32_t c = 0xFFFFFFFF;
  for(; *s; s++){ c ^= (uint8_t)*s; for(uint8_t k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & (0 - (c & 1))); }
  return ~c;
}

/*  ---------- PNG ---------- */

static uint32_t be32(const uint8_t* p){ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static uint8_t paeth(int a, int b, int c){
  int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
  return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

/*  Розпакувати PNG у RGBA8888. Черезрядковий (interlace) не підтримуємо —
    серед значків станцій він трапляється рідко.  */
static bool pngDecode(const uint8_t* d, size_t n, uint8_t** outRGBA, uint32_t* W, uint32_t* H){
  static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
  if(n < 33 || memcmp(d, sig, 8)) return false;
  uint32_t w = 0, h = 0; uint8_t bd = 0, ct = 0, il = 0;
  uint8_t pal[256][4]; uint16_t npal = 0;
  int32_t trGray = -1; int32_t trR = -1, trG = -1, trB = -1;
  for(int i = 0; i < 256; i++){ pal[i][0] = pal[i][1] = pal[i][2] = 0; pal[i][3] = 255; }
  /*  спершу рахуємо загальний розмір IDAT  */
  size_t idatLen = 0;
  for(size_t p = 8; p + 12 <= n; ){
    uint32_t len = be32(d + p); const uint8_t* t = d + p + 4; const uint8_t* c = d + p + 8;
    if(p + 12 + len > n) break;
    if(!memcmp(t, "IHDR", 4) && len >= 13){ w = be32(c); h = be32(c + 4); bd = c[8]; ct = c[9]; il = c[12]; }
    else if(!memcmp(t, "PLTE", 4)){ npal = len / 3; if(npal > 256) npal = 256; for(uint16_t k = 0; k < npal; k++){ pal[k][0] = c[k*3]; pal[k][1] = c[k*3+1]; pal[k][2] = c[k*3+2]; } }
    else if(!memcmp(t, "tRNS", 4)){
      if(ct == 3){ for(uint32_t k = 0; k < len && k < 256; k++) pal[k][3] = c[k]; }
      else if(ct == 0 && len >= 2) trGray = (c[0] << 8) | c[1];
      else if(ct == 2 && len >= 6){ trR = (c[0] << 8) | c[1]; trG = (c[2] << 8) | c[3]; trB = (c[4] << 8) | c[5]; }
    }
    else if(!memcmp(t, "IDAT", 4)) idatLen += len;
    else if(!memcmp(t, "IEND", 4)) break;
    p += 12 + len;
  }
  if(!w || !h || il || (uint64_t)w * h > LG_MAXPX) return false;
  uint8_t ch = ct == 0 ? 1 : ct == 2 ? 3 : ct == 3 ? 1 : ct == 4 ? 2 : ct == 6 ? 4 : 0;
  if(!ch || !(bd == 1 || bd == 2 || bd == 4 || bd == 8 || bd == 16)) return false;
  uint32_t bitsPx = ch * bd, stride = (w * bitsPx + 7) / 8, bpp = bitsPx >= 8 ? bitsPx / 8 : 1;
  size_t rawLen = (size_t)h * (stride + 1);
  uint8_t* idat = (uint8_t*)ps_malloc(idatLen + 1);
  uint8_t* raw  = (uint8_t*)ps_malloc(rawLen);
  uint8_t* rgba = (uint8_t*)ps_malloc((size_t)w * h * 4);
  bool ok = false;
  if(idat && raw && rgba){
    size_t q = 0;
    for(size_t p = 8; p + 12 <= n; ){
      uint32_t len = be32(d + p);
      if(p + 12 + len > n) break;
      if(!memcmp(d + p + 4, "IDAT", 4)){ memcpy(idat + q, d + p + 8, len); q += len; }
      p += 12 + len;
    }
    /*  Розпаковувач (~11 КБ) — у PSRAM. tinfl_decompress_mem_to_mem тримає його на
        стеку, через це задачі логотипа був потрібен стек 24 КБ, а на ES3C35P суцільного
        шматка внутрішньої пам'яті такого розміру часто немає — логотипи не вантажились.
        Робить те саме, що й tinfl_decompress_mem_to_mem (ПЗУ ESP32-S3).  */
    size_t got = 0;
    tinfl_decompressor* dz = (tinfl_decompressor*)ps_malloc(sizeof(tinfl_decompressor));
    if(dz){
      tinfl_init(dz);
      size_t inB = idatLen, outB = rawLen;
      const tinfl_status st = tinfl_decompress(dz, (const mz_uint8*)idat, &inB, (mz_uint8*)raw, (mz_uint8*)raw, &outB,
                                               TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
      if(st == TINFL_STATUS_DONE) got = outB;
      free(dz);
    }
    if(got == rawLen){
      /*  зняти фільтри рядків (None, Sub, Up, Average, Paeth)  */
      uint8_t* prev = nullptr;
      for(uint32_t y = 0; y < h; y++){
        uint8_t* row = raw + (size_t)y * (stride + 1);
        uint8_t f = row[0]; uint8_t* s = row + 1;
        for(uint32_t x = 0; x < stride; x++){
          uint8_t a = x >= bpp ? s[x - bpp] : 0, b = prev ? prev[x] : 0, c = (prev && x >= bpp) ? prev[x - bpp] : 0;
          switch(f){ case 1: s[x] += a; break; case 2: s[x] += b; break; case 3: s[x] += (a + b) >> 1; break; case 4: s[x] += paeth(a, b, c); break; default: break; }
        }
        prev = s;
        /*  у RGBA  */
        for(uint32_t x = 0; x < w; x++){
          uint8_t* o = rgba + ((size_t)y * w + x) * 4;
          auto sample = [&](uint32_t idx)->uint16_t{         /* idx — номер каналу в рядку */
            if(bd == 16) return s[idx * 2];                   /* старший байт */
            if(bd == 8)  return s[idx];
            uint32_t bit = idx * bd; uint8_t v = (s[bit >> 3] >> (8 - bd - (bit & 7))) & ((1 << bd) - 1);
            return v;
          };
          uint16_t v0 = sample(x * ch);
          if(ct == 3){ uint8_t* pp = pal[v0 & 255]; o[0] = pp[0]; o[1] = pp[1]; o[2] = pp[2]; o[3] = pp[3]; continue; }
          uint8_t scale = bd < 8 ? (255 / ((1 << bd) - 1)) : 1;
          if(ct == 0 || ct == 4){
            uint8_t g = (uint8_t)(v0 * scale); o[0] = o[1] = o[2] = g;
            o[3] = ct == 4 ? (uint8_t)sample(x * ch + 1) : ((trGray >= 0 && (int32_t)v0 == (bd == 16 ? (trGray >> 8) : trGray)) ? 0 : 255);
          }else{
            o[0] = (uint8_t)v0; o[1] = (uint8_t)sample(x * ch + 1); o[2] = (uint8_t)sample(x * ch + 2);
            o[3] = ct == 6 ? (uint8_t)sample(x * ch + 3) : ((trR >= 0 && o[0] == (bd == 16 ? trR >> 8 : trR) && o[1] == (bd == 16 ? trG >> 8 : trG) && o[2] == (bd == 16 ? trB >> 8 : trB)) ? 0 : 255);
          }
        }
      }
      ok = true;
    }
  }
  if(idat) free(idat);
  if(raw) free(raw);
  if(!ok){ if(rgba) free(rgba); return false; }
  *outRGBA = rgba; *W = w; *H = h;
  return true;
}

/*  Якість межі на банері (останній розбір): частка площі й видовженість знайденої межі.
    Логотип на банері — пляма посередині; якщо «логотип» займає майже весь банер чи лишився
    смугою — це не логотип, а картинка-заставка (Radio Jazz, Авторадіо: у списку станцій
    виходив темний прямокутник із крихітним знаком).  */
volatile float yoBoxCover = 0, yoBoxAspect = 0;
static void boxQuality(uint32_t w, uint32_t h, uint32_t cw, uint32_t ch){
  yoBoxCover = (w && h) ? (float)cw * ch / ((float)w * h) : 1.0f;
  yoBoxAspect = (cw && ch) ? (cw > ch ? (float)cw / ch : (float)ch / cw) : 99.0f;
}

/*  Підкладка під прозоре. Біла — як було; але світлий знак на прозорому тлі (білий
    логотип «Наше Радіо» 300×110) на білому зникав: у списку стояв порожній білий
    квадрат. Такий кладемо на темну підкладку, близьку до тла списку.  */
#define LG_DARK_R 40
#define LG_DARK_G 52
#define LG_DARK_B 66
struct LgAlpha {
  uint32_t n = 0, tr = 0, op = 0, lum = 0;
  void add(int r, int g, int b, int a){ n++; if(a < 128) tr++; else{ op++; lum += (uint32_t)(r * 299 + g * 587 + b * 114) / 1000; } }
  bool dark() const { return op && tr * 10 >= n && lum / op >= 200; }   /* ≥ 10 % прозорого, решта — світла */
};

/*  Розмір самого знака в готовій клітинці LOGO_S×LOGO_S: усе, що помітно відрізняється
    від кутової точки. Напис «Громадське радіо» 250×44 дає в клітинці 45×7 точок — не
    прочитати; білий знак «Наше Радіо» на темній підкладці — 13×12. Поріг нижчий, ніж
    у межах банера (150), щоб блідий знак не вважався порожнім.  */
void yoLogoContent(const uint16_t* px, int& bw, int& bh){
  auto rgb = [](uint16_t c, int& r, int& g, int& b){ r = (c >> 11) * 255 / 31; g = ((c >> 5) & 63) * 255 / 63; b = (c & 31) * 255 / 31; };
  int br, bg, bb; rgb(px[0], br, bg, bb);
  int x0 = LOGO_S, y0 = LOGO_S, x1 = -1, y1 = -1;
  for(int y = 0; y < LOGO_S; y++)
    for(int x = 0; x < LOGO_S; x++){
      int r, g, b; rgb(px[y * LOGO_S + x], r, g, b);
      if(abs(r - br) + abs(g - bg) + abs(b - bb) > 80){ if(x < x0) x0 = x; if(x > x1) x1 = x; if(y < y0) y0 = y; if(y > y1) y1 = y; }
    }
  bw = x1 < 0 ? 0 : x1 - x0 + 1; bh = y1 < 0 ? 0 : y1 - y0 + 1;
}
/*  Чи прочитається: не нижче 10 точок і не менше 24 у довжину.  */
bool yoLogoReadable(int bw, int bh){ return (bw < bh ? bw : bh) >= 10 && (bw > bh ? bw : bh) >= 24; }

/*  ---------- межі логотипа на банері ----------
    Фон — колір кутової точки (прозоре — на білому); логотип — усе, що від нього
    помітно відрізняється. Повертає прямокутник з полем ~6 %; нічого — уся картинка.  */
void yoContentBox(const uint8_t* px, uint32_t w, uint32_t h, uint8_t chans, uint32_t& cx0, uint32_t& cy0, uint32_t& cw, uint32_t& ch, uint16_t* bg565){
  auto rgbAt = [&](uint32_t x, uint32_t y, int& r, int& g, int& b){
    const uint8_t* p = px + ((size_t)y * w + x) * chans;
    const int a = chans == 4 ? p[3] : 255;
    r = (p[0] * a + 255 * (255 - a)) / 255; g = (p[1] * a + 255 * (255 - a)) / 255; b = (p[2] * a + 255 * (255 - a)) / 255;
  };
  int br, bg, bb; rgbAt(0, 0, br, bg, bb);
  if(bg565) *bg565 = (uint16_t)(((br & 0xF8) << 8) | ((bg & 0xFC) << 3) | (bb >> 3));
  uint32_t x0 = w, y0 = h, x1 = 0, y1 = 0;
  for(uint32_t y = 0; y < h; y++)
    for(uint32_t x = 0; x < w; x++){
      int r, g, b; rgbAt(x, y, r, g, b);
      /*  150: білий логотип на темному банері (~600) — так, градієнт фону (< 90) — ні  */
      if(abs(r - br) + abs(g - bg) + abs(b - bb) > 150){ if(x < x0) x0 = x; if(x > x1) x1 = x; if(y < y0) y0 = y; if(y > y1) y1 = y; }
    }
  if(x1 < x0 || y1 < y0){ cx0 = cy0 = 0; cw = w; ch = h; boxQuality(w, h, cw, ch); return; }
  const uint32_t pad = ((x1 - x0) > (y1 - y0) ? (x1 - x0) : (y1 - y0)) * 6 / 100 + 1;
  cx0 = x0 > pad ? x0 - pad : 0; cy0 = y0 > pad ? y0 - pad : 0;
  const uint32_t ex = x1 + pad + 1 < w ? x1 + pad + 1 : w, ey = y1 + pad + 1 < h ? y1 + pad + 1 : h;
  cw = ex - cx0; ch = ey - cy0;
  boxQuality(w, h, cw, ch);
}

/*  ---------- великий PNG: потоком, одразу у W×H ----------
    Цілком такий не розпакуєш (1920×1920 — 11 МБ рядків), тож: розпаковувач із
    вікном 32 КБ (як у zlib), рядок за рядком знімаємо фільтр (потрібні лише
    поточний і попередній рядок) і віддаємо рядок RGBA. Через рядок (Adam7) — ні.
    Банер og:image (crop < 0) — двічі: спершу межі логотипа, потім сам логотип.  */
static void cropRect(uint32_t sw, uint32_t sh, float crop, uint32_t& cx0, uint32_t& cy0, uint32_t& cw, uint32_t& ch);

struct PngInfo { uint32_t w, h; uint8_t bd, ct, ch; uint8_t pal[256][4]; };

static bool pngHead(const uint8_t* d, size_t n, PngInfo& pi){
  static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
  if(n < 33 || memcmp(d, sig, 8)) return false;
  uint8_t il = 0; pi.w = pi.h = 0; pi.bd = pi.ct = 0;
  for(int i = 0; i < 256; i++){ pi.pal[i][0] = pi.pal[i][1] = pi.pal[i][2] = 0; pi.pal[i][3] = 255; }
  for(size_t p = 8; p + 12 <= n; ){
    uint32_t len = be32(d + p); const uint8_t* t = d + p + 4; const uint8_t* c = d + p + 8;
    if(p + 12 + len > n) break;
    if(!memcmp(t, "IHDR", 4) && len >= 13){ pi.w = be32(c); pi.h = be32(c + 4); pi.bd = c[8]; pi.ct = c[9]; il = c[12]; }
    else if(!memcmp(t, "PLTE", 4)){ uint16_t np = len / 3; if(np > 256) np = 256; for(uint16_t k = 0; k < np; k++){ pi.pal[k][0] = c[k*3]; pi.pal[k][1] = c[k*3+1]; pi.pal[k][2] = c[k*3+2]; } }
    else if(!memcmp(t, "tRNS", 4) && pi.ct == 3){ for(uint32_t k = 0; k < len && k < 256; k++) pi.pal[k][3] = c[k]; }
    else if(!memcmp(t, "IEND", 4)) break;
    p += 12 + len;
  }
  if(!pi.w || !pi.h || il || pi.w > 8192 || pi.h > 8192) return false;
  pi.ch = pi.ct == 0 ? 1 : pi.ct == 2 ? 3 : pi.ct == 3 ? 1 : pi.ct == 4 ? 2 : pi.ct == 6 ? 4 : 0;
  return pi.ch && (pi.bd == 1 || pi.bd == 2 || pi.bd == 4 || pi.bd == 8 || pi.bd == 16);
}

/*  Рядки PNG по черзі: cb(y, rgba) — w точок по 4 байти.  */
template<typename F> static bool pngRows(const uint8_t* d, size_t n, const PngInfo& pi, F cb){
  const uint32_t w = pi.w, h = pi.h, bd = pi.bd, ct = pi.ct, ch = pi.ch;
  const uint32_t bitsPx = ch * bd, stride = (w * bitsPx + 7) / 8, bpp = bitsPx >= 8 ? bitsPx / 8 : 1;
  uint8_t* cur = (uint8_t*)ps_malloc(stride + 1);
  uint8_t* prv = (uint8_t*)heap_caps_calloc(stride, 1, MALLOC_CAP_SPIRAM);
  uint8_t* rgba = (uint8_t*)ps_malloc((size_t)w * 4);
  uint8_t* dict = (uint8_t*)ps_malloc(TINFL_LZ_DICT_SIZE);
  tinfl_decompressor* dz = (tinfl_decompressor*)ps_malloc(sizeof(tinfl_decompressor));
  bool ok = false;
  if(cur && prv && rgba && dict && dz){
    tinfl_init(dz);
    uint32_t y = 0, fill = 0;
    size_t dictOfs = 0;
    bool done = false, bad = false;
    auto sample = [&](const uint8_t* s, uint32_t idx)->uint16_t{
      if(bd == 16) return s[idx * 2];
      if(bd == 8)  return s[idx];
      uint32_t bit = idx * bd; return (s[bit >> 3] >> (8 - bd - (bit & 7))) & ((1 << bd) - 1);
    };
    auto rowDone = [&](){
      uint8_t ft = cur[0]; uint8_t* s = cur + 1;
      for(uint32_t x = 0; x < stride; x++){
        uint8_t a = x >= bpp ? s[x - bpp] : 0, b = prv[x], c = x >= bpp ? prv[x - bpp] : 0;
        switch(ft){ case 1: s[x] += a; break; case 2: s[x] += b; break; case 3: s[x] += (a + b) >> 1; break; case 4: s[x] += paeth(a, b, c); break; default: break; }
      }
      const uint8_t scale = bd < 8 ? (255 / ((1 << bd) - 1)) : 1;
      for(uint32_t x = 0; x < w; x++){
        uint8_t* o = rgba + x * 4;
        const uint16_t v0 = sample(s, x * ch);
        if(ct == 3){ const uint8_t* pp = pi.pal[v0 & 255]; o[0] = pp[0]; o[1] = pp[1]; o[2] = pp[2]; o[3] = pp[3]; }
        else if(ct == 0 || ct == 4){ o[0] = o[1] = o[2] = (uint8_t)(v0 * scale); o[3] = ct == 4 ? (uint8_t)sample(s, x * ch + 1) : 255; }
        else{ o[0] = (uint8_t)v0; o[1] = (uint8_t)sample(s, x * ch + 1); o[2] = (uint8_t)sample(s, x * ch + 2); o[3] = ct == 6 ? (uint8_t)sample(s, x * ch + 3) : 255; }
      }
      cb(y, rgba);
      memcpy(prv, s, stride);
      y++; fill = 0;
      if(!(y & 31)) vTaskDelay(1);                 /* віддати ядро — задача простою й сторож */
    };
    for(size_t p = 8; p + 12 <= n && !done && !bad; ){
      uint32_t len = be32(d + p);
      if(p + 12 + len > n) break;
      if(!memcmp(d + p + 4, "IDAT", 4)){
        const uint8_t* in = d + p + 8; size_t left = len;
        for(;;){
          size_t inB = left, outB = TINFL_LZ_DICT_SIZE - dictOfs;
          tinfl_status st = tinfl_decompress(dz, in, &inB, dict, dict + dictOfs, &outB, TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT);
          in += inB; left -= inB;
          for(size_t k = 0; k < outB && y < h; k++){
            cur[fill++] = dict[dictOfs + k];
            if(fill == stride + 1) rowDone();
          }
          dictOfs = (dictOfs + outB) & (TINFL_LZ_DICT_SIZE - 1);
          if(st == TINFL_STATUS_DONE){ done = true; break; }
          if(st < 0){ bad = true; break; }
          if(st == TINFL_STATUS_NEEDS_MORE_INPUT && !left) break;
          if(y >= h){ done = true; break; }
        }
      }else if(!memcmp(d + p + 4, "IEND", 4)) break;
      p += 12 + len;
    }
    ok = y >= h;
  }
  if(cur) free(cur);
  if(prv) free(prv);
  if(rgba) free(rgba);
  if(dict) free(dict);
  if(dz) free(dz);
  return ok;
}

static bool pngFitBig(const uint8_t* d, size_t n, uint16_t* out, int W, int H, float crop = 0){
  PngInfo* pi = (PngInfo*)ps_malloc(sizeof(PngInfo));
  if(!pi || !pngHead(d, n, *pi)){ if(pi) free(pi); return false; }
  const uint32_t w = pi->w, h = pi->h;
  uint32_t cx0, cy0, cw, chh;
  uint16_t fill = 0xFFFF;                         /* поля навколо вписаного — білі (чи темні — під світлий знак) */
  const bool banner = crop < 0;
  if(crop < 0){
    /*  межі логотипа: перший прохід — лише порівняння з кольором кутової точки  */
    int br = -1, bg = 0, bb = 0;
    uint32_t x0 = w, y0 = h, x1 = 0, y1 = 0;
    pngRows(d, n, *pi, [&](uint32_t y, const uint8_t* row){
      for(uint32_t x = 0; x < w; x++){
        const uint8_t* q = row + x * 4; const int a = q[3];
        const int r = (q[0] * a + 255 * (255 - a)) / 255, g = (q[1] * a + 255 * (255 - a)) / 255, b = (q[2] * a + 255 * (255 - a)) / 255;
        if(br < 0){ br = r; bg = g; bb = b; }
        if(abs(r - br) + abs(g - bg) + abs(b - bb) > 150){ if(x < x0) x0 = x; if(x > x1) x1 = x; if(y < y0) y0 = y; if(y > y1) y1 = y; }
      }
    });
    if(br >= 0) fill = (uint16_t)(((br & 0xF8) << 8) | ((bg & 0xFC) << 3) | (bb >> 3));   /* поля — кольором банера */
    if(x1 >= x0 && y1 >= y0){
      const uint32_t pad = ((x1 - x0) > (y1 - y0) ? (x1 - x0) : (y1 - y0)) * 6 / 100 + 1;
      cx0 = x0 > pad ? x0 - pad : 0; cy0 = y0 > pad ? y0 - pad : 0;
      cw = (x1 + pad + 1 < w ? x1 + pad + 1 : w) - cx0; chh = (y1 + pad + 1 < h ? y1 + pad + 1 : h) - cy0;
    }else{ cx0 = cy0 = 0; cw = w; chh = h; }
    boxQuality(w, h, cw, chh);
  }else cropRect(w, h, crop, cx0, cy0, cw, chh);
  /*  область джерела і куди вона лягає в W×H (з полями, як fitRGBA)  */
  const float f = fminf((float)W / cw, (float)H / chh);
  int dw = (int)(cw * f), dh = (int)(chh * f); if(dw < 1) dw = 1; if(dh < 1) dh = 1;
  const int ox = (W - dw) / 2, oy = (H - dh) / 2;
  /*  на точку: сума кольору, помноженого на непрозорість, сума непрозорості, кількість —
      підкладку під прозоре вибираємо, коли вже видно всю картинку  */
  uint32_t* acc = (uint32_t*)heap_caps_calloc((size_t)dw * dh * 5, 4, MALLOC_CAP_SPIRAM);
  uint16_t* xmap = (uint16_t*)ps_malloc((size_t)w * 2);
  bool ok = false;
  LgAlpha st;
  if(acc && xmap){
    for(uint32_t x = 0; x < w; x++) xmap[x] = (x < cx0 || x >= cx0 + cw) ? 0xFFFF : (uint16_t)((uint64_t)(x - cx0) * dw / cw);
    ok = pngRows(d, n, *pi, [&](uint32_t y, const uint8_t* row){
      if(y < cy0 || y >= cy0 + chh) return;
      const uint32_t py = (uint64_t)(y - cy0) * dh / chh;
      for(uint32_t x = 0; x < w; x++){
        if(xmap[x] == 0xFFFF) continue;
        const uint8_t* q = row + x * 4; const uint32_t al = q[3];
        uint32_t* a5 = acc + ((size_t)py * dw + xmap[x]) * 5;
        a5[0] += q[0] * al; a5[1] += q[1] * al; a5[2] += q[2] * al; a5[3] += al; a5[4]++;
        st.add(q[0], q[1], q[2], al);
      }
    });
    if(ok){
      const bool dk = st.dark();
      const uint32_t mr = dk ? LG_DARK_R : 255, mg = dk ? LG_DARK_G : 255, mb = dk ? LG_DARK_B : 255;
      if(dk && !banner) fill = (uint16_t)(((mr & 0xF8) << 8) | ((mg & 0xFC) << 3) | (mb >> 3));
      for(int yy = 0; yy < H; yy++)
        for(int xx = 0; xx < W; xx++){
          uint16_t pxl = fill;
          const int sx = xx - ox, sy = yy - oy;
          if(sx >= 0 && sy >= 0 && sx < dw && sy < dh){
            const uint32_t* a5 = acc + ((size_t)sy * dw + sx) * 5;
            if(a5[4]){
              const uint32_t k = a5[4] * 255, rest = k - a5[3];
              const uint32_t r = (a5[0] + mr * rest) / k, g = (a5[1] + mg * rest) / k, b = (a5[2] + mb * rest) / k;
              pxl = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
            }
          }
          out[yy * W + xx] = pxl;
        }
    }
  }
  if(acc) free(acc);
  if(xmap) free(xmap);
  free(pi);
  return ok;
}

/*  ---------- ICO: беремо найбільший значок, усередині PNG або BMP ---------- */

static bool icoDecode(const uint8_t* d, size_t n, uint8_t** outRGBA, uint32_t* W, uint32_t* H){
  if(n < 22 || d[0] || d[1] || d[2] != 1 || d[3]) return false;
  uint16_t cnt = d[4] | (d[5] << 8);
  int best = -1; uint32_t bestW = 0;
  for(uint16_t i = 0; i < cnt && 6 + i * 16 + 16 <= n; i++){
    const uint8_t* e = d + 6 + i * 16;
    uint32_t w = e[0] ? e[0] : 256;
    if(w > bestW){ bestW = w; best = i; }
  }
  if(best < 0) return false;
  const uint8_t* e = d + 6 + best * 16;
  uint32_t size = e[8] | (e[9] << 8) | (e[10] << 16) | ((uint32_t)e[11] << 24);
  uint32_t off  = e[12] | (e[13] << 8) | (e[14] << 16) | ((uint32_t)e[15] << 24);
  if(off + size > n) return false;
  const uint8_t* b = d + off;
  if(size > 8 && b[0] == 0x89 && b[1] == 'P') return pngDecode(b, size, outRGBA, W, H);
  /*  BMP без файлового заголовка: висота подвоєна (там ще маска)  */
  if(size < 40) return false;
  int32_t w = (int32_t)(b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24));
  int32_t h = (int32_t)(b[8] | (b[9] << 8) | (b[10] << 16) | ((uint32_t)b[11] << 24)) / 2;
  uint16_t bpp = b[14] | (b[15] << 8);
  uint32_t hs = b[0] | (b[1] << 8);
  if(w <= 0 || h <= 0 || w > 512 || h > 512) return false;
  if(bpp != 32 && bpp != 24){
    /*  палітра (1/4/8 біт) чи 16 біт — збираємо справжній BMP-файл (заголовок 14 Б,
        висота навпіл — без маски) і віддаємо stb_image  */
    uint8_t* f = (uint8_t*)ps_malloc(size + 14);
    if(!f) return false;
    memcpy(f + 14, b, size);
    const uint32_t clr = (b[32] | (b[33] << 8) | (b[34] << 16) | ((uint32_t)b[35] << 24));
    const uint32_t pal = bpp <= 8 ? 4u * (clr ? clr : (1u << bpp)) : 0;
    const uint32_t offBits = 14 + hs + pal, fsz = size + 14;
    f[0] = 'B'; f[1] = 'M';
    f[2] = fsz; f[3] = fsz >> 8; f[4] = fsz >> 16; f[5] = fsz >> 24;
    f[6] = f[7] = f[8] = f[9] = 0;
    f[10] = offBits; f[11] = offBits >> 8; f[12] = offBits >> 16; f[13] = offBits >> 24;
    const int32_t hh = h;                          /* у DIB висота з маскою — пишемо справжню */
    f[14 + 8] = hh; f[14 + 9] = hh >> 8; f[14 + 10] = hh >> 16; f[14 + 11] = hh >> 24;
    const bool ok = yoStbDecode(f, fsz, outRGBA, W, H);
    free(f);
    return ok;
  }
  uint32_t rowB = ((w * bpp / 8) + 3) & ~3u;
  if(hs + rowB * h > size) return false;
  uint8_t* rgba = (uint8_t*)ps_malloc((size_t)w * h * 4);
  if(!rgba) return false;
  for(int32_t y = 0; y < h; y++){
    const uint8_t* s = b + hs + (size_t)(h - 1 - y) * rowB;
    for(int32_t x = 0; x < w; x++){
      uint8_t* o = rgba + ((size_t)y * w + x) * 4;
      o[0] = s[x * bpp / 8 + 2]; o[1] = s[x * bpp / 8 + 1]; o[2] = s[x * bpp / 8];
      o[3] = bpp == 32 ? s[x * 4 + 3] : 255;
    }
  }
  *outRGBA = rgba; *W = w; *H = h;
  return true;
}

/*  Область джерела: уся або з середини з пропорцією crop.  */
static void cropRect(uint32_t sw, uint32_t sh, float crop, uint32_t& cx0, uint32_t& cy0, uint32_t& cw, uint32_t& ch){
  cx0 = cy0 = 0; cw = sw; ch = sh;
  if(crop <= 0) return;
  if((float)sw / sh > crop){ cw = (uint32_t)(sh * crop); cx0 = (sw - cw) / 2; }
  else{ ch = (uint32_t)(sw / crop); cy0 = (sh - ch) / 2; }
  if(!cw) cw = 1; if(!ch) ch = 1;
}

/*  RGBA -> W x H RGB565, прозоре на білому (світлий знак — на темному), з усередненням пікселів.  */
static void fitRGBA(const uint8_t* rgba, uint32_t sw0, uint32_t sh0, uint16_t* out, int W, int H, float crop = 0){
  uint32_t cx0, cy0, sw, sh;
  uint16_t fill = 0xFFFF;
  if(crop < 0) yoContentBox(rgba, sw0, sh0, 4, cx0, cy0, sw, sh, &fill);   /* банер — лише сам логотип, поля — його фоном */
  else cropRect(sw0, sh0, crop, cx0, cy0, sw, sh);
  LgAlpha st;
  for(uint32_t y = cy0; y < cy0 + sh; y++)
    for(uint32_t x = cx0; x < cx0 + sw; x++){ const uint8_t* p = rgba + ((size_t)y * sw0 + x) * 4; st.add(p[0], p[1], p[2], p[3]); }
  const bool dk = st.dark();
  const uint32_t mr = dk ? LG_DARK_R : 255, mg = dk ? LG_DARK_G : 255, mb = dk ? LG_DARK_B : 255;
  if(dk && crop >= 0) fill = (uint16_t)(((mr & 0xF8) << 8) | ((mg & 0xFC) << 3) | (mb >> 3));
  float f = fminf((float)W / sw, (float)H / sh);
  int dw = (int)(sw * f), dh = (int)(sh * f);
  if(dw < 1) dw = 1; if(dh < 1) dh = 1;
  int ox = (W - dw) / 2, oy = (H - dh) / 2;
  for(int y = 0; y < H; y++)
    for(int x = 0; x < W; x++){
      uint32_t r = ((fill >> 11) & 31) * 255 / 31, g = ((fill >> 5) & 63) * 255 / 63, b = (fill & 31) * 255 / 31;
      int sx = x - ox, sy = y - oy;
      if(sx >= 0 && sy >= 0 && sx < dw && sy < dh){
        int x0 = sx * sw / dw, x1 = (sx + 1) * sw / dw; if(x1 <= x0) x1 = x0 + 1;
        int y0 = sy * sh / dh, y1 = (sy + 1) * sh / dh; if(y1 <= y0) y1 = y0 + 1;
        uint32_t R = 0, G = 0, B = 0, A = 0, k = 0;
        for(int yy = y0; yy < y1; yy++)
          for(int xx = x0; xx < x1; xx++){
            const uint8_t* p = rgba + ((size_t)(yy + cy0) * sw0 + xx + cx0) * 4;
            const uint32_t a = p[3];
            R += p[0] * a; G += p[1] * a; B += p[2] * a; A += a; k++;
          }
        const uint32_t kk = k * 255, rest = kk - A;
        r = (R + mr * rest) / kk; g = (G + mg * rest) / kk; b = (B + mb * rest) / kk;
      }
      out[y * W + x] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    }
}

bool yoImageFit(const uint8_t* d, size_t n, uint16_t* out, int W, int H, float crop){
  /*  спершу свої (швидкі, перевірені); не впорались — stb_image (прогресивний JPEG,
      PNG через рядок, GIF, ICO з палітрою)  */
  if(n > 3 && d[0] == 0xFF && d[1] == 0xD8 && yoJpegFit(d, n, out, W, H, crop)) return true;
  uint8_t* rgba = nullptr; uint32_t w = 0, h = 0;
  bool ok = false;
  if(n > 24 && d[0] == 0x89 && d[1] == 'P'){
    const uint32_t pw = be32(d + 16), ph = be32(d + 20);              /* IHDR — завжди перший */
    if((uint64_t)pw * ph > 256 * 1024) return pngFitBig(d, n, out, W, H, crop);
    ok = pngDecode(d, n, &rgba, &w, &h);
  }
  else if(n > 4 && !d[0] && !d[1] && d[2] == 1 && !d[3]) ok = icoDecode(d, n, &rgba, &w, &h);
  if(!ok){ if(rgba){ free(rgba); rgba = nullptr; } ok = yoStbDecode(d, n, &rgba, &w, &h); }
  if(!ok) return false;
  fitRGBA(rgba, w, h, out, W, H, crop);
  free(rgba);
  return true;
}

/*  ---------- мережа ---------- */

static void urlEnc(const char* s, char* o, size_t cap){
  static const char* hx = "0123456789ABCDEF";
  size_t k = 0;
  for(; *s && k + 4 < cap; s++){
    uint8_t c = (uint8_t)*s;
    if(isalnum(c) && c < 0x80) o[k++] = c;
    else if(c == '-' || c == '_' || c == '.' || c == '~') o[k++] = c;
    else { o[k++] = '%'; o[k++] = hx[c >> 4]; o[k++] = hx[c & 15]; }
  }
  o[k] = 0;
}

static uint16_t s_tries = 0;                 /* скільки повторів знадобилось у цьому пошуку */
/*  Найменше вільної внутрішньої пам'яті під час останнього запиту з TLS (mbedTLS тут бере
    лише внутрішню: CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC) — поруч із TLS самої станції.  */
static volatile uint32_t s_tlsMin = UINT32_MAX;

/*  Одна спроба GET у буфер PSRAM. Довжина; 0 — сервер відповів «нема» (4xx);
    -1 — мережа або сервер тимчасово не впорався (5xx, 429) — варто повторити.
    whole — потрібен увесь файл (картинка): більший за буфер одразу 0. Обрізаний PNG
    не розпакується, а спроба йшла секунди: «Люкс FM Золоті хіти» — 5,5 МБ, качалось
    1,5 МБ і ще 12 с розпаковувалось до помилки.  */
/*  Пам'ять. Мережа (TLS, буфери TCP і Wi-Fi) бере лише внутрішню. Прохід логотипів поруч зі
    станцією по https і веб-сторінкою опускав її мінімум до 4,5 КБ (27.10.2026) — на краю, де вже
    не зможе з'єднатись сама станція чи впаде веб-сервер. Тож пошук логотипа поступається:
    перед запитом чекає, доки вільно ≥ LG_MEMSTART, а посеред завантаження, коли лишилось
    < LG_MEMLOW, кидає його (як мережевий збій — спробуємо згодом).  */
#define LG_MEMSTART (26 * 1024)
#define LG_MEMLOW   (10 * 1024)

extern volatile uint32_t yoWebHeavyAt;          /* extras/yoWebApi.cpp */
/*  Веб-сторінка саме вантажиться (файли, логотипи): тоді пошук не починаємо, а прохід «усі
    логотипи» між станціями завершує задачу (16 КБ її стека — внутрішня пам'ять) і продовжує
    згодом. З проходом, що лише чекав, мінімум однаково опускався до 8,8 КБ.  */
static bool webBusy(){ const uint32_t t = yoWebHeavyAt; return t && millis() - t < 4000; }

static int httpGetOnce(const char* url0, uint8_t* buf, size_t cap, bool whole){
  /*  сторінка почала вантажитись посеред пошуку — почекати, до 20 с  */
  for(uint8_t w = 0; w < 100 && webBusy(); w++) vTaskDelay(pdMS_TO_TICKS(200));
  for(uint8_t w = 0; w < 50 && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < LG_MEMSTART; w++) vTaskDelay(pdMS_TO_TICKS(200));
  if(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < LG_MEMSTART){
    Serial.printf("##LOGO#\tмало пам'яті (%u Б) — запит відкладено\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return -1;
  }
  /*  Переадресації — самі: HTTPClient лишається на тому ж типі з'єднання, і з
      http:// на https:// (301 radiomaria.org.ua, sw-radio.com) не переходив —
      логотип вважався «нема».  */
  char url[480];
  strlcpy(url, url0, sizeof(url));
  for(uint8_t hop = 0; hop < 5; hop++){
    const bool tls = !strncmp(url, "https", 5);
    if(tls) heap_caps_monitor_local_minimum_free_size_start();
    struct TlsMin { bool on; ~TlsMin(){ if(on){ const uint32_t lm = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
                                                    heap_caps_monitor_local_minimum_free_size_stop(); if(lm < s_tlsMin) s_tlsMin = lm; } } } tm{tls};
    WiFiClient plain; WiFiClientSecure sec; sec.setInsecure();
    HTTPClient http;
    /*  HTTP/1.0: інакше сервер може віддати відповідь частинами (chunked), а потік з
        getStreamPtr() — сирий, із заголовками частин усередині. Значок hromadske.radio
        починався з «51b7\r\n» замість PNG і не розпаковувався; так само міг рватися
        JSON каталогу чи сторінка сайту.  */
    http.useHTTP10(true);
    http.setTimeout(6000); http.setConnectTimeout(6000);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setUserAgent("PotuzhneRadio/1.0");
    if(!(tls ? http.begin(sec, url) : http.begin(plain, url))) return -1;
    static const char* HK[] = { "Location" };
    http.collectHeaders(HK, 1);
    int code = http.GET();
    if(code == 301 || code == 302 || code == 303 || code == 307 || code == 308){
      String loc = http.header("Location");
      http.end();
      if(!loc.length()) return 0;
      char nu[480];
      if(loc.startsWith("http://") || loc.startsWith("https://")) strlcpy(nu, loc.c_str(), sizeof(nu));
      else{
        const char* hs = strstr(url, "://"); hs = hs ? hs + 3 : url;
        const char* pe = strchr(hs, '/');
        const size_t baseLen = pe ? (size_t)(pe - url) : strlen(url);    /* «схема://хост» */
        if(loc.startsWith("//")) snprintf(nu, sizeof(nu), "%s:%s", tls ? "https" : "http", loc.c_str());
        else if(loc.startsWith("/")) snprintf(nu, sizeof(nu), "%.*s%s", (int)baseLen, url, loc.c_str());
        else{
          const char* ls = strrchr(url, '/');
          const size_t dirLen = (ls && ls >= url + baseLen) ? (size_t)(ls - url + 1) : baseLen;
          snprintf(nu, sizeof(nu), "%.*s%s%s", (int)dirLen, url, (ls && ls >= url + baseLen) ? "" : "/", loc.c_str());
        }
      }
      strlcpy(url, nu, sizeof(url));
      continue;
    }
    if(code != 200){ http.end(); return (code > 0 && code < 500 && code != 429) ? 0 : -1; }
    WiFiClient* st = http.getStreamPtr();
    int total = http.getSize();
    if(whole && total > (int)cap){
      Serial.printf("##LOGO#\tзавеликий файл (%d КБ) — пропускаю\n", total / 1024);
      http.end(); return 0;
    }
    size_t len = 0;
    /*  великий значок — довше: 10 с + секунда на кожні 20 КБ (PNG 1 МБ через TLS іде ~30–50 с)  */
    uint32_t deadline = millis() + 10000 + (total > 0 ? (uint32_t)total / 20 : 0);
    while(len < cap && (int32_t)(deadline - millis()) > 0 && (total < 0 || (int)len < total)){
      if(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < LG_MEMLOW){
        Serial.printf("##LOGO#\tмало пам'яті (%u Б) — завантаження перервано\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        http.end(); return -1;
      }
      int av = st->available();
      if(!av){ if(!st->connected()) break; vTaskDelay(pdMS_TO_TICKS(5)); continue; }
      len += st->read(buf + len, (size_t)av > cap - len ? cap - len : av);
    }
    http.end();
    if(whole && total < 0 && len >= cap) return 0;  /* розмір не сказали, а буфер уже повний — обрізаний */
    return (int)len;
  }
  return 0;                                        /* забагато переадресацій */
}

/*  GET з повторами. Каталог radio-browser (усі адреси — один сервер) 25.10.2026
    скидав ~70 % з'єднань («Connection reset by peer»): з однієї спроби логотип
    майже не знаходився, а після мережевої невдачі пошук до зміни станції не
    повторюється. Тож до 6 спроб із паузою 1,5 с.  */
static volatile bool s_inNet = false;          /* іде запит (TLS тримає пам'ять) */
bool YoLogos::netBusy() const { return s_inNet; }

/*  tries: 6 — для каталогу (його сервер рве ~70 % з'єднань), 2 — для значків на інших
    сайтах: невдалі спроби до мертвого сервера йдуть під час гри, і кожна заважала
    потоку (26.10.2026: провали звуку саме на «Радіо Промінь: мережа … (повторів 5)»).  */
static int httpGet(const char* url, uint8_t* buf, size_t cap, uint8_t tries = 6, bool whole = false){
  struct NetMark { NetMark(){ s_inNet = true; } ~NetMark(){ s_inNet = false; } } mark;
  for(uint8_t a = 0; a < tries; a++){
    if(a){ s_tries++; vTaskDelay(pdMS_TO_TICKS(1500)); }
    int n = httpGetOnce(url, buf, cap, whole);
    if(n >= 0) return n;
    if(WiFi.status() != WL_CONNECTED) break;
  }
  return -1;
}

/*  Перше непорожнє "favicon" у відповіді каталогу; якщо byName — лише у
    станції з точно такою ж назвою (щоб не взяти чужий логотип).  */
/*  ---------- розбір відповіді каталогу ----------
    radio-browser віддає масив плоских об'єктів: {"name":"…","url":"…",
    "url_resolved":"…","favicon":"…","countrycode":"UA",…}. Беремо з об'єкта
    потрібні поля (з \" і \/ усередині).  */
static bool jsonField(const char* obj, const char* objEnd, const char* key, char* out, size_t cap){
  char pat[24]; snprintf(pat, sizeof(pat), "\"%s\":\"", key);
  const char* p = strstr(obj, pat);
  out[0] = 0;
  if(!p || p > objEnd) return false;
  p += strlen(pat);
  size_t k = 0;
  for(; p < objEnd && *p && *p != '"'; p++){
    if(*p == '\\' && p[1]){ p++; }
    if(k + 1 < cap) out[k++] = *p;
  }
  out[k] = 0;
  return k > 0;
}

/*  Назва для порівняння: лише літери й цифри, малими (латиниця й кирилиця).  */
static void normName(const char* s, char* o, size_t cap){
  size_t k = 0;
  const uint8_t* u = (const uint8_t*)s;
  while(*u && k + 3 < cap){
    uint32_t c = *u;
    if(c < 0x80){ u++; if(c >= 'A' && c <= 'Z') c += 32; if((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) o[k++] = (char)c; continue; }
    if((c & 0xE0) == 0xC0 && u[1]){
      c = ((c & 0x1F) << 6) | (u[1] & 0x3F); u += 2;
      if(c >= 0x410 && c <= 0x42F) c += 0x20;             /* А–Я → а–я */
      else if(c >= 0x400 && c <= 0x40F) c += 0x50;        /* Ё, Є, І, Ї… → малі */
      else if(c == 0x490) c = 0x491;                      /* Ґ → ґ */
      if(c == 0x451) c = 0x435;                           /* ё → е */
      if(c >= 0x430 && c <= 0x4FF){ o[k++] = (char)(0xC0 | (c >> 6)); o[k++] = (char)(0x80 | (c & 0x3F)); }
      continue;
    }
    u++;
  }
  o[k] = 0;
}

static void hostOf(const char* url, char* o, size_t cap){
  const char* p = strstr(url, "://");
  p = p ? p + 3 : url;
  size_t k = 0;
  for(; *p && *p != '/' && *p != ':' && *p != '?' && k + 1 < cap; p++) o[k++] = (char)tolower((unsigned char)*p);
  o[k] = 0;
}

/*  Країна за доменом потоку: «online.kissfm.ua» → «UA». IP-адреса й загальні
    домени (com, net, fm, io…) країни не дають — тоді й не перевіряємо.  */
static bool tldCountry(const char* host, char cc[3]){
  const char* d = strrchr(host, '.');
  if(!d || strlen(d + 1) != 2 || !isalpha((unsigned char)d[1]) || !isalpha((unsigned char)d[2])) return false;
  static const char* const GEN[] = { "fm", "io", "tv", "me", "co", "ai", "cc", "to", "ws", "la", "am" };
  for(const char* g : GEN) if(!strcmp(d + 1, g)) return false;
  cc[0] = (char)toupper((unsigned char)d[1]); cc[1] = (char)toupper((unsigned char)d[2]); cc[2] = 0;
  return true;
}

/*  Спільне «змістовне» слово в назвах (≥ 4 літер, не «radio/радіо/fm…») — щоб
    збіг за сервером потоку не дав логотип чужої станції на тому ж сервері.  */
static bool shareWord(const char* a, const char* b){
  char wa[100], wb[100];
  auto words = [](const char* s, char* out, size_t cap){
    size_t k = 0; bool sep = true;
    for(const uint8_t* u = (const uint8_t*)s; *u && k + 2 < cap; u++){
      bool alnum = (*u >= 0x80) || isalnum(*u);
      if(alnum){ out[k++] = (char)((*u < 0x80) ? tolower(*u) : *u); sep = false; }
      else if(!sep){ out[k++] = ' '; sep = true; }
    }
    out[k] = 0;
  };
  words(a, wa, sizeof(wa)); words(b, wb, sizeof(wb));
  static const char* const GEN[] = { "radio", "радіо", "радио", "online", "stream", "music", "christian" };
  for(char* t = strtok(wa, " "); t; t = strtok(nullptr, " ")){
    if(strlen(t) < 4) continue;
    bool gen = false; for(const char* g : GEN) if(!strcmp(t, g)) gen = true;
    if(gen) continue;
    char nt[60]; normName(t, nt, sizeof(nt));
    char nb[110]; normName(b, nb, sizeof(nb));
    if(nt[0] && strstr(nb, nt)) return true;
  }
  return false;
}

/*  ---------- значок сайту станції (кого немає в каталозі) ----------
    Домен потоку («online.kissfm.ua» → «kissfm.ua»), лише якщо його назва схожа на
    назву станції (інакше це сервіс мовлення — tvstitch, radioking… — і значок чужий).
    Кирилицю порівнюємо транслітом: «Наше Радіо» → nasheradio, «Мелодія FM» → melodiafm.  */
static void translit(const char* s, char* o, size_t cap){
  static const struct { uint16_t c; const char* l; } T[] = {
    {0x430,"a"},{0x431,"b"},{0x432,"v"},{0x433,"h"},{0x491,"g"},{0x434,"d"},{0x435,"e"},{0x454,"e"},{0x436,"zh"},
    {0x437,"z"},{0x438,"y"},{0x456,"i"},{0x457,"i"},{0x439,"i"},{0x43A,"k"},{0x43B,"l"},{0x43C,"m"},{0x43D,"n"},
    {0x43E,"o"},{0x43F,"p"},{0x440,"r"},{0x441,"s"},{0x442,"t"},{0x443,"u"},{0x444,"f"},{0x445,"kh"},{0x446,"ts"},
    {0x447,"ch"},{0x448,"sh"},{0x449,"shch"},{0x44C,""},{0x44E,"u"},{0x44F,"a"},{0x44B,"y"},{0x44D,"e"},{0x451,"e"},{0x44A,""} };
  char n[200]; normName(s, n, sizeof(n));
  size_t k = 0;
  for(const uint8_t* u = (const uint8_t*)n; *u && k + 5 < cap; ){
    if(*u < 0x80){ o[k++] = (char)*u++; continue; }
    uint16_t c = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F); u += 2;
    for(const auto& t : T) if(t.c == c){ for(const char* q = t.l; *q && k + 1 < cap; q++) o[k++] = *q; break; }
  }
  o[k] = 0;
}

/*  «online.kissfm.ua» → «kissfm.ua»; «onair.lviv.fm» → «lviv.fm»; IP — порожньо.  */
static void siteOf(const char* host, char* o, size_t cap, char* label, size_t lcap){
  o[0] = label[0] = 0;
  if(!host[0] || isdigit((unsigned char)host[strlen(host) - 1])) return;      /* IP */
  const char* lab[8]; uint8_t n = 0; lab[n++] = host;
  for(const char* p = host; *p && n < 8; p++) if(*p == '.') lab[n++] = p + 1;
  if(n < 2) return;
  /*  друге з кінця — загальне (com.ua, net.ua, gov.ua, co.za…) — беремо три мітки  */
  static const char* const SLD[] = { "com.", "net.", "org.", "gov.", "co.", "in." };
  uint8_t take = 2;
  for(const char* g : SLD) if(n >= 3 && !strncmp(lab[n - 2], g, strlen(g))) take = 3;
  if(n < take) return;
  strlcpy(o, lab[n - take], cap);
  const char* d = strchr(lab[n - take], '.');
  size_t ll = d ? (size_t)(d - lab[n - take]) : strlen(lab[n - take]);
  if(ll >= lcap) ll = lcap - 1;
  memcpy(label, lab[n - take], ll); label[ll] = 0;
}

static bool siteMatches(const char* label, const char* name){
  char tl[200], lb[40];
  size_t k = 0;
  for(const char* p = label; *p && k + 1 < sizeof(lb); p++) if(isalnum((unsigned char)*p)) lb[k++] = (char)tolower((unsigned char)*p);
  lb[k] = 0;
  if(k < 4) return false;
  translit(name, tl, sizeof(tl));
  return strstr(tl, lb) != nullptr || (strlen(tl) >= 6 && strstr(lb, tl) != nullptr);
}

/*  Значки з головної сторінки сайту: <link rel="…icon…" href= sizes=> і
    <meta property="og:image" content=>. WEBP і SVG — повз (не розпакуємо).  */
struct SiteIcon { char url[200]; int16_t size; bool og; };

static bool tagAttr(const char* t, const char* te, const char* name, char* out, size_t cap){
  out[0] = 0;
  const size_t nl = strlen(name);
  for(const char* p = t; p + nl + 2 < te; p++){
    if(strncasecmp(p, name, nl) || p[nl] != '=' || (p > t && isalnum((unsigned char)p[-1]))) continue;
    const char* v = p + nl + 1;
    char q = (*v == '"' || *v == '\'') ? *v++ : ' ';
    size_t k = 0;
    while(v < te && *v != q && *v != '>' && !(q == ' ' && isspace((unsigned char)*v)) && k + 1 < cap) out[k++] = *v++;
    out[k] = 0;
    return k > 0;
  }
  return false;
}

static void absUrl(const char* base, const char* u, char* out, size_t cap){
  if(!strncmp(u, "http://", 7) || !strncmp(u, "https://", 8)) strlcpy(out, u, cap);
  else if(!strncmp(u, "//", 2)) snprintf(out, cap, "https:%s", u);
  else if(u[0] == '/') snprintf(out, cap, "%s%s", base, u);
  else snprintf(out, cap, "%s/%s", base, u);
}

/*  Розширення самого файла (до «?» і «#»): мініатюри Wikimedia мають «.svg» посередині адреси
    («…/Ukrainske_Radio_(2022).svg/960px-….svg.png»), а самі — PNG. Через пошук «.svg» будь-де
    логотип «Українського радіо» з каталогу відкидався.  */
static bool urlExt(const char* u, const char* ext){
  const size_t el = strlen(ext), n = strcspn(u, "?#");
  return n >= el && !strncasecmp(u + n - el, ext, el);
}

static uint8_t siteIcons(const char* html, size_t n, const char* base, SiteIcon* out, uint8_t cap){
  uint8_t k = 0;
  char a[220], b[220], u[220];
  for(const char* p = html; p < html + n && k < cap; p++){
    if(*p != '<') continue;
    const bool isLink = !strncasecmp(p + 1, "link", 4), isMeta = !strncasecmp(p + 1, "meta", 4);
    if(!isLink && !isMeta) continue;
    const char* te = (const char*)memchr(p, '>', (html + n) - p);
    if(!te || te - p > 800) continue;
    if(isLink){
      if(!tagAttr(p, te, "rel", a, sizeof(a)) || !strcasestr(a, "icon")) continue;
      if(!tagAttr(p, te, "href", u, sizeof(u))) continue;
      int16_t sz = 0;
      if(tagAttr(p, te, "sizes", b, sizeof(b))) sz = (int16_t)atoi(b);
      if(!sz) sz = strcasestr(a, "apple") ? 180 : (strcasestr(u, ".ico") ? 32 : 64);
      if(urlExt(u, ".svg") || urlExt(u, ".webp") || !strncmp(u, "data:", 5)) continue;
      absUrl(base, u, out[k].url, sizeof(out[k].url)); out[k].size = sz; out[k].og = false; k++;
    }else{
      if(!(tagAttr(p, te, "property", a, sizeof(a)) || tagAttr(p, te, "name", a, sizeof(a))) || strcasecmp(a, "og:image")) continue;
      if(!tagAttr(p, te, "content", u, sizeof(u)) || urlExt(u, ".svg") || urlExt(u, ".webp")) continue;
      absUrl(base, u, out[k].url, sizeof(out[k].url)); out[k].size = 0; out[k].og = true; k++;
    }
    p = te;
  }
  return k;
}

/*  Малими (латиниця й кирилиця), решту лишаємо — для пошуку фрази.  */
static void lowerUtf8(const char* s, char* o, size_t cap){
  size_t k = 0;
  const uint8_t* u = (const uint8_t*)s;
  while(*u && k + 3 < cap){
    if(*u < 0x80){ o[k++] = (char)tolower(*u); u++; continue; }
    if((u[0] & 0xE0) == 0xC0 && u[1]){
      uint32_t c = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F); u += 2;
      if(c >= 0x410 && c <= 0x42F) c += 0x20; else if(c >= 0x400 && c <= 0x40F) c += 0x50; else if(c == 0x490) c = 0x491;
      o[k++] = (char)(0xC0 | (c >> 6)); o[k++] = (char)(0x80 | (c & 0x3F));
      continue;
    }
    o[k++] = (char)*u++;
  }
  o[k] = 0;
}

/*  Назва станції в назві з каталогу цілою фразою: «Радіо Промінь / Radio Promin»
    містить «Радіо Промінь»; а «Flux FM» — не «Lux FM» (межа слова).  */
static bool phraseIn(const char* hay, const char* name){
  char h[160], n[100];
  lowerUtf8(hay, h, sizeof(h)); lowerUtf8(name, n, sizeof(n));
  const size_t nl = strlen(n);
  if(nl < 6) return false;
  for(const char* p = strstr(h, n); p; p = strstr(p + 1, n)){
    const bool before = p == h || (!isalnum((unsigned char)p[-1]) && (uint8_t)p[-1] < 0x80);
    const char e = p[nl];
    const bool after = !e || (!isalnum((unsigned char)e) && (uint8_t)e < 0x80);
    if(before && after) return true;
  }
  return false;
}

/*  Вибрати значок з відповіді. mode 0 — byurl (будь-яка станція з тим самим
    потоком); 1 — пошук за назвою: та сама назва (і країна за доменом, коли вона є)
    або той самий сервер потоку зі спільним словом у назві. Пропускаємо SVG
    (його не розпакуємо). skip — скільки придатних пропустити (наступний кандидат,
    якщо попередній значок не розпакувався чи вийшов дрібним); лік — через усі
    проходи (раніше скидався на кожному, і кандидати з другого проходу губились).
    field — "favicon" або "homepage" (домашня сторінка тієї ж станції: з неї
    значок шукаємо, коли потік іде з IP чи з чужого домену).  */
static bool pickEntry(const char* json, const char* myName, const char* myUrl, uint8_t mode, uint8_t skip, const char* field, char* out, size_t cap){
  const bool icon = !strcmp(field, "favicon");
  char nm[100], my[100], fav[256], cc[4], u1[200], h1[64], h2[64], myHost[64], myCc[3];
  normName(myName, my, sizeof(my));
  hostOf(myUrl, myHost, sizeof(myHost));
  const bool hasCc = tldCountry(myHost, myCc);
  uint8_t seen = 0;
  for(uint8_t pass = 0; pass < (mode ? 3 : 1); pass++){
    const char* p = json;
    while((p = strchr(p, '{')) != nullptr){
      const char* e = strchr(p, '}');
      if(!e) break;
      if(jsonField(p, e, field, fav, sizeof(fav)) && !strncmp(fav, "http", 4) && !(icon && urlExt(fav, ".svg"))){
        bool ok = mode == 0;
        if(mode){
          jsonField(p, e, "name", u1, sizeof(u1));
          normName(u1, nm, sizeof(nm));
          if(pass == 0){
            ok = my[0] && !strcmp(nm, my);
            if(ok && hasCc && jsonField(p, e, "countrycode", cc, sizeof(cc)) && strcmp(cc, myCc)) ok = false;   /* «Radio Roks» з Латвії */
          }else if(pass == 2){
            /*  наша назва цілою фразою в назві з каталогу — і та сама країна (коли домен її дає)  */
            ok = phraseIn(u1, myName);
            if(ok && hasCc && (!jsonField(p, e, "countrycode", cc, sizeof(cc)) || strcmp(cc, myCc))) ok = false;
          }else{
            char a[200];
            jsonField(p, e, "url", a, sizeof(a)); hostOf(a, h1, sizeof(h1));
            jsonField(p, e, "url_resolved", a, sizeof(a)); hostOf(a, h2, sizeof(h2));
            ok = myHost[0] && (!strcmp(h1, myHost) || !strcmp(h2, myHost)) && shareWord(myName, u1);
          }
        }
        if(ok && seen++ >= skip){ strlcpy(out, fav, cap); return true; }
      }
      p = e + 1;
    }
  }
  return false;
}

bool YoLogos::want(const char* url, const char* name){
  if(_busy || WiFi.status() != WL_CONNECTED || webBusy()) return false;   /* не почався — плеєр спитає ще раз за кілька секунд */
  strlcpy(_url, url, sizeof(_url));
  strlcpy(_name, name, sizeof(_name));
  _crc = crc(url);
  _busy = true;
  /*  16 КБ, а не 24: розпаковувач PNG тепер у PSRAM (див. pngDecode). Суцільний шматок
      на 24 КБ у внутрішній пам'яті ES3C35P під час гри буває не завжди.  */
  /*  Пріоритет 0 — як у задачі простою: розпакування великого значка займає ядро на
      секунди, і з пріоритетом 1 задача простою ядра 0 не діставала часу — сторож задач
      перезапускав радіо (25.10.2026, PNG 1920×1920).  */
  if(xTaskCreatePinnedToCore(_task, "logo", 16384, this, 0, nullptr, 0) != pdPASS){ _busy = false; return false; }
  return true;
}

uint16_t YoLogos::forget(){
  /*  «Шукати знову»: список «нема» — порожній, файл списку — геть (один запис).
      Виконується в головному циклі: файлів не обходимо (це робить задача, noLoad).  */
  const uint16_t n = s_noN;
  s_noN = 0; s_noDirty = false; s_passDone = s_passDone0 = false;
  if(SPIFFS.exists(LG_NOLIST)) SPIFFS.remove(LG_NOLIST);
  return n;
}

/*  Один пошук для _url/_name (з задачі): у SPIFFS — .565 (є) або позначка «нема».
    Повертає 1 — знайдено, 0 — нема, -1 — мережа.
    Порядок: каталог за адресою потоку → каталог за назвою → сайт станції (за адресою
    потоку, а коли там IP чи чужий домен — домашня сторінка з каталогу). Кожен значок
    перевіряємо вже в клітинці: знак, який не прочитати (напис 45×7), — шукаємо далі.  */
int YoLogos::_one(){
  s_tries = 0;
  s_tlsMin = UINT32_MAX;
  _crc = crc(_url);
  uint8_t* buf = (uint8_t*)ps_malloc(LG_MAXDL + 1);
  uint8_t* js = (uint8_t*)ps_malloc(64 * 1024 + 1);
  uint16_t* px = (uint16_t*)ps_malloc(LOGO_S * LOGO_S * 2);
  SiteIcon* ic = (SiteIcon*)ps_malloc(sizeof(SiteIcon) * 10);
  char fav[256] = {0}, cand[256], home[200] = {0};
  uint32_t tried[20]; uint8_t nTried = 0;
  bool found = false, netFail = false;

  /*  один кандидат: скачати, вписати в клітинку, перевірити, зберегти  */
  auto tryIcon = [&](const char* u, float crop) -> bool {
    const uint32_t c = crc(u);
    for(uint8_t i = 0; i < nTried; i++) if(tried[i] == c) return false;      /* цей уже пробували */
    if(nTried < sizeof(tried) / sizeof(tried[0])) tried[nTried++] = c;
    const int n = httpGet(u, buf, LG_MAXDL, 2, true);
    if(n <= 64) return false;                 /* не відповів, «нема», завеликий — наступний */
    yoBoxCover = 0; yoBoxAspect = 0;
    if(!yoImageFit(buf, n, px, LOGO_S, LOGO_S, crop)) return false;
    if(crop < 0 && (yoBoxCover > 0.75f || yoBoxAspect > 2.2f)){
      Serial.printf("##LOGO#\t%s: банер без окремого логотипа (займає %.0f %%, видовжений %.1f) — шукаю далі\n", _name, yoBoxCover * 100, yoBoxAspect);
      return false;
    }
    int bw, bh; yoLogoContent(px, bw, bh);
    if(!yoLogoReadable(bw, bh)){
      Serial.printf("##LOGO#\t%s: знак у клітинці лише %d×%d — не прочитати, шукаю далі (%s)\n", _name, bw, bh, u);
      return false;
    }
    char path[28]; snprintf(path, sizeof(path), "/logo/%08x.565", (unsigned)_crc);
    File o = SPIFFS.open(path, "w");
    if(!o) return false;
    o.write((uint8_t*)px, LOGO_S * LOGO_S * 2); o.close();
    strlcpy(fav, u, sizeof(fav));
    return true;
  };

  /*  Wikimedia віддає зменшені копії лише стандартних ширин (798px — помилка 400):
      «…/thumb/…/798px-Файл.png» → «…/250px-Файл.png»  */
  auto wikiThumb = [](char* f, size_t cap){
    if(!strstr(f, "wikimedia.org") || !strstr(f, "/thumb/")) return;      /* upload.… і thumb.… */
    char* seg = strrchr(f, '/');
    char* pp = seg ? strstr(seg, "px-") : nullptr;
    if(!pp || pp <= seg + 1) return;
    for(char* q = seg + 1; q < pp; q++) if(!isdigit((unsigned char)*q)) return;
    char tail[200]; strlcpy(tail, pp, sizeof(tail));
    snprintf(seg + 1, cap - (seg + 1 - f), "250%s", tail);
  };

  /*  значки з головної сторінки сайту (<link>, og:image), далі служба Google  */
  auto trySite = [&](const char* site) -> bool {
    uint8_t ni = 0;
    char base[96];
    for(uint8_t v = 0; v < 2 && !ni; v++){
      snprintf(base, sizeof(base), v ? "https://www.%s" : "https://%s", site);
      char hp[110]; snprintf(hp, sizeof(hp), "%s/", base);
      const int n = httpGet(hp, buf, 512 * 1024, 2);
      if(n > 0){ buf[n] = 0; ni = siteIcons((const char*)buf, n, base, ic, 10); }
    }
    /*  порядок: великі значки (≥ 96) → середина og:image → дрібніші → служба Google  */
    int8_t order[12]; uint8_t no = 0;
    for(int16_t lim : { (int16_t)96, (int16_t)0 })
      for(uint8_t pass = 0; pass < 2; pass++)
        for(uint8_t i = 0; i < ni; i++){
          const bool big = !ic[i].og && ic[i].size >= 96;
          if(lim == 96 && pass == 0 && big) order[no++] = i;
          if(lim == 96 && pass == 1 && ic[i].og) order[no++] = i;
          if(lim == 0 && pass == 0 && !ic[i].og && !big && ic[i].size >= 32) order[no++] = i;
        }
    /*  з великих — спершу найбільший  */
    for(uint8_t i = 0; i < no; i++) for(uint8_t j = i + 1; j < no; j++)
      if(!ic[order[i]].og && !ic[order[j]].og && ic[order[i]].size >= 96 && ic[order[j]].size > ic[order[i]].size){ int8_t t = order[i]; order[i] = order[j]; order[j] = t; }
    /*  до п'яти зі сторінки, тоді служба Google  */
    if(no > 5) no = 5;
    order[no++] = -1;
    for(uint8_t k = 0; k < no; k++){
      char cu[220]; float crop = 0;
      if(order[k] < 0) snprintf(cu, sizeof(cu), "https://www.google.com/s2/favicons?domain=%s&sz=128", site);
      else{ strlcpy(cu, ic[order[k]].url, sizeof(cu)); if(ic[order[k]].og) crop = -1; }   /* банер — межі логотипа */
      if(tryIcon(cu, crop)) return true;
    }
    return false;
  };

  if(buf && js && px && ic){
    char q[460], enc[400];
    int n = 0;
    /*  1) той самий потік (як записано, і з http↔https)  */
    for(uint8_t v = 0; v < 2 && !found; v++){
      char u2[210];
      if(v == 0) strlcpy(u2, _url, sizeof(u2));
      else if(!strncmp(_url, "http://", 7)){ snprintf(u2, sizeof(u2), "https://%s", _url + 7); }
      else if(!strncmp(_url, "https://", 8)){ snprintf(u2, sizeof(u2), "http://%s", _url + 8); }
      else break;
      urlEnc(u2, enc, sizeof(enc));
      snprintf(q, sizeof(q), LG_API "byurl?url=%s", enc);
      n = httpGet(q, js, 64 * 1024);
      if(n < 0){ netFail = true; break; }
      js[n] = 0;
      if(!home[0]) pickEntry((char*)js, _name, _url, 0, 0, "homepage", home, sizeof(home));
      bool any = false;
      for(uint8_t k = 0; k < 4 && !found && pickEntry((char*)js, _name, _url, 0, k, "favicon", cand, sizeof(cand)); k++){
        any = true; wikiThumb(cand, sizeof(cand)); found = tryIcon(cand, 0);
      }
      if(any) break;                          /* у каталозі є — другий варіант адреси не потрібен */
    }
    /*  2) пошук за назвою: та сама назва (та країна) або той самий сервер потоку  */
    if(!found && !netFail){
      urlEnc(_name, enc, sizeof(enc));
      snprintf(q, sizeof(q), LG_API "search?name=%s&limit=40&hidebroken=true", enc);
      n = httpGet(q, js, 64 * 1024);
      if(n < 0) netFail = true;
      else{
        js[n] = 0;
        if(!home[0]) pickEntry((char*)js, _name, _url, 1, 0, "homepage", home, sizeof(home));
        for(uint8_t k = 0; k < 5 && !found && pickEntry((char*)js, _name, _url, 1, k, "favicon", cand, sizeof(cand)); k++){
          wikiThumb(cand, sizeof(cand)); found = tryIcon(cand, 0);
        }
      }
    }
    /*  3) сайт станції: за адресою потоку; коли там IP чи чужий домен — домашня сторінка
          з каталогу («Громадське радіо» грає з IP, а значок — на hromadske.radio)  */
    if(!found && !netFail){
      char host[64], site[64], site0[64] = {0}, label[40];
      for(uint8_t src = 0; src < 2 && !found; src++){
        if(src && !home[0]) break;
        hostOf(src ? home : _url, host, sizeof(host));
        siteOf(host, site, sizeof(site), label, sizeof(label));
        if(!site[0] || !siteMatches(label, _name) || !strcmp(site, site0)) continue;
        strlcpy(site0, site, sizeof(site0));
        found = trySite(site);
      }
    }
  }
  const bool mem = buf && js && px && ic;
  if(buf) free(buf);
  if(js) free(js);
  if(px) free(px);
  if(ic) free(ic);
  const int result = found ? 1 : (netFail || !mem) ? -1 : 0;
  if(result == 0) noAdd(_crc);                 /* нема — у список (у файл — наприкінці, одним записом) */
  Serial.printf("##LOGO#\t%s: %s %s (повторів %u; внутрішньої пам'яті під час TLS лишалось %d Б, стек вільний %u Б)\n", _name,
                result > 0 ? "знайдено" : result == 0 ? "нема" : "мережа", fav, s_tries,
                s_tlsMin == UINT32_MAX ? -1 : (int)s_tlsMin, (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  if(result > 0) _ver++;
  return result;
}

void YoLogos::_task(void* arg){
  YoLogos* s = (YoLogos*)arg;
  noLoad();
  if(!noHas(crc(s->_url))){
    if(!s->_quick) vTaskDelay(pdMS_TO_TICKS(3000));   /* нехай станція спершу підключиться */
    waitStream();                                     /* і набере запас у буфері */
    s->_one();
    noSave();
  }
  s->_quick = false;
  s->_busy = false;
  vTaskDelete(nullptr);
}

/*  ---------- перевірка одного значка (logotest) ---------- */
bool YoLogos::test(const char* favUrl){
  if(_busy || WiFi.status() != WL_CONNECTED) return false;
  if(!_tu) _tu = (char*)ps_malloc(512);
  if(!_tu) return false;
  strlcpy(_tu, favUrl, 512);
  _busy = true;
  if(xTaskCreatePinnedToCore(_testTask, "logotest", 16384, this, 0, nullptr, 0) != pdPASS){ _busy = false; return false; }
  return true;
}

void YoLogos::_testTask(void* arg){
  YoLogos* s = (YoLogos*)arg;
  s_tries = 0;
  s_tlsMin = UINT32_MAX;
  uint8_t* buf = (uint8_t*)ps_malloc(LG_MAXDL + 1);
  uint16_t* px = (uint16_t*)ps_malloc(LOGO_S * LOGO_S * 2);
  int n = buf ? httpGet(s->_tu, buf, LG_MAXDL) : -1;
  const int64_t t0 = esp_timer_get_time();
  bool ok = n > 0 && px && yoImageFit(buf, n, px, LOGO_S, LOGO_S);
  const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
  Serial.printf("LOGOTEST %s\n  байтів %d (повторів %u), підпис %02X %02X %02X %02X, розпаковано: %s за %u мкс\n",
                s->_tu, n, s_tries, n > 3 ? buf[0] : 0, n > 3 ? buf[1] : 0, n > 3 ? buf[2] : 0, n > 3 ? buf[3] : 0,
                ok ? "так" : "НІ", (unsigned)us);
  Serial.printf("  внутрішньої пам'яті під час TLS лишалось %d Б (зараз %u), стек вільний %u Б\n", s_tlsMin == UINT32_MAX ? -1 : (int)s_tlsMin,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  if(buf) free(buf);
  if(px) free(px);
  s->_busy = false;
  vTaskDelete(nullptr);
}

/*  ---------- «нема логотипа»: один список, а не файл на станцію ----------
    Власник 26.10.2026: «SPIFFS не должен использоваться как кеш или хранилищем с
    постоянной перезаписью — это медленно и быстро убьёт устройство». Тож: список
    станцій без логотипа — у PSRAM і одним файлом /logo/no.lst (перший рядок — «done»,
    якщо повний прохід уже був без мережевих збоїв), пишеться лише коли список
    змінився. Раніше — окремий .no на кожну станцію плюс позначки проходів (і я їх
    кілька разів переписував). Старі файли переносяться в список один раз і стираються.
    Запис у флеш вимикає кеш обох ядер на десятки мс — ще й тому його має бути мало.  */

static bool noHas(uint32_t c){ for(uint16_t i = 0; i < s_noN; i++) if(s_no[i] == c) return true; return false; }
static void noAdd(uint32_t c){
  if(noHas(c)) return;
  if(s_noN >= s_noCap){
    uint16_t nc = s_noCap ? s_noCap * 2 : 64;
    uint32_t* n = (uint32_t*)heap_caps_realloc(s_no, (size_t)nc * 4, MALLOC_CAP_SPIRAM);
    if(!n) return;
    s_no = n; s_noCap = nc;
  }
  s_no[s_noN++] = c; s_noDirty = true;
}

/*  З задачі логотипа (не з головного циклу: обхід SPIFFS довгий).  */
/*  Один раз після зміни правил: збережені логотипи, які в клітинці не прочитати (білий
    квадрат «Наше Радіо», напис «Громадське радіо» 45×7, крихітний знак на банері), —
    стерти; список «нема» скинути, щоб і там, де раніше не знайшли, шукалось новими
    правилами. Прохід «усі логотипи» після цього — знову (лише по відсутніх).  */
static void logoReview(){
  const uint16_t CAP = 200;
  char (*bad)[32] = (char (*)[32])ps_malloc((size_t)CAP * 32);
  uint16_t* px = (uint16_t*)ps_malloc(LOGO_S * LOGO_S * 2);
  uint16_t k = 0, all = 0;
  if(bad && px){
    File dir = SPIFFS.open("/");
    if(dir){
      for(File e = dir.openNextFile(); e && k < CAP; e = dir.openNextFile()){
        const char* nm = e.path();
        const size_t l = strlen(nm);
        if(strstr(nm, "/logo/") && l > 4 && !strcmp(nm + l - 4, ".565") && e.size() == LOGO_S * LOGO_S * 2
           && e.read((uint8_t*)px, LOGO_S * LOGO_S * 2) == LOGO_S * LOGO_S * 2){
          all++;
          int bw, bh; yoLogoContent(px, bw, bh);
          if(!yoLogoReadable(bw, bh)){
            Serial.printf("##LOGO#\tперегляд: %s — знак %d×%d, не прочитати — стираю\n", nm, bw, bh);
            strlcpy(bad[k++], nm, 32);
          }
        }
        e.close();
      }
      dir.close();
    }
    for(uint16_t i = 0; i < k; i++) SPIFFS.remove(bad[i]);
  }
  if(bad) free(bad);
  if(px) free(px);
  Serial.printf("##LOGO#\tперегляд логотипів за новими правилами: усього %u, стерто %u, позначок «нема» скинуто %u\n",
                (unsigned)all, (unsigned)k, (unsigned)s_noN);
  s_noN = 0; s_passDone = false; s_noDirty = true;
  if(k) logos.bump();
}

static void noLoad(){
  if(s_noLoaded) return;
  File f = SPIFFS.open(LG_NOLIST, "r");
  uint8_t rules = 1;                            /* списку немає — збережене могло лишитися від старих правил */
  if(f){
    String h = f.readStringUntil('\n'); h.trim();
    s_passDone = s_passDone0 = h.startsWith("done");
    if(h.length() > 5) rules = (uint8_t)atoi(h.c_str() + 5);
    while(f.available()){
      String l = f.readStringUntil('\n'); l.trim();
      if(l.length() == 8) noAdd((uint32_t)strtoul(l.c_str(), nullptr, 16));
    }
    f.close();
    s_noDirty = false;
  }
  /*  перенести старі позначки (по файлу на станцію, позначки проходів) — один раз  */
  const uint16_t CAP = 200;
  char (*old)[32] = (char (*)[32])ps_malloc((size_t)CAP * 32);
  uint16_t k = 0;
  if(old){
    File dir = SPIFFS.open("/");
    if(dir){
      for(File e = dir.openNextFile(); e && k < CAP; e = dir.openNextFile()){
        const char* nm = e.path();
        if(strstr(nm, "/logo/") && (strstr(nm, ".no") || strstr(nm, "/all")) && !strstr(nm, "no.lst")) strlcpy(old[k++], nm, 32);
        e.close();
      }
      dir.close();
    }
    for(uint16_t i = 0; i < k; i++){
      const char* b = strrchr(old[i], '/') + 1;
      if(strlen(b) == 11 && !strcmp(b + 8, ".no")){ char hx[9]; memcpy(hx, b, 8); hx[8] = 0; noAdd((uint32_t)strtoul(hx, nullptr, 16)); }
      SPIFFS.remove(old[i]);
    }
    free(old);
    if(k) Serial.printf("##LOGO#\tстарі позначки перенесено в один список: %u файлів\n", (unsigned)k);
  }
  if(rules < LG_RULES) logoReview();
  s_noLoaded = true;
}

static void noSave(){
  if(!s_noDirty && s_passDone == s_passDone0) return;
  File o = SPIFFS.open(LG_NOLIST, "w");
  if(!o) return;
  o.printf("%s %u\n", s_passDone ? "done" : "todo", (unsigned)LG_RULES);
  for(uint16_t i = 0; i < s_noN; i++) o.printf("%08x\n", (unsigned)s_no[i]);
  o.close();
  s_noDirty = false; s_passDone0 = s_passDone;
}

bool YoLogos::isNo(uint32_t c) const { return s_noLoaded && noHas(c); }

/*  Не заважати потоку: мережевий пошук — лише коли в буфері потоку є запас
    (≥ 2 с звуку, від 16 до 48 КБ). 26.10.2026 обидва провали звуку за 5 хв
    припали саме на пошук логотипа (буфер потоку — 2 КБ).  */
extern volatile bool yoDnsWarm;                  /* core/network.cpp */
static void waitStream(){
  /*  перший запит DNS після появи адреси робить мережа сама (див. dnsWarm) — чекаємо  */
  for(uint16_t i = 0; i < 100 && !yoDnsWarm; i++) vTaskDelay(pdMS_TO_TICKS(100));
  for(uint16_t i = 0; i < 300; i++){             /* до хвилини */
    if(!player.isRunning() || player.remoteStationName) return;
    uint32_t need = (uint32_t)config.station.bitrate * 1000 / 8 * 2;
    if(need < 16384) need = 16384;
    if(need > 49152) need = 49152;
    if(player.inBufferFilled() >= need) return;
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

/*  ---------- усі станції списку ---------- */
/*  Список «назва<Tab>адреса<Tab>гучність» — у PSRAM, один раз за прохід.  */
struct LgSt { char name[64]; char url[200]; };

/*  Окрема задача на весь прохід: головний цикл (дотик, звук) файлів не читає.
    Прохід — раз за увімкнення і лише доки не було повного без мережевих збоїв
    (у списку «done»). Повторів кожні 10 хвилин більше немає: мережа, що не
    відповіла, — наступного увімкнення.  */
void YoLogos::_allTask(void* arg){
  YoLogos* s = (YoLogos*)arg;
  noLoad();
  if(s->_bForget){ s->_bForget = false; }
  if(s->_bWipe){
    /*  «logowipe» (налагодження): усі логотипи наново — стерти файли по одному, з паузами
        (кожне стирання — запис у флеш), список «нема» — порожній, прохід — знову.  */
    s->_bWipe = false;
    const uint16_t CAP = 200;
    char (*all)[32] = (char (*)[32])ps_malloc((size_t)CAP * 32);
    uint16_t k = 0;
    if(all){
      File dir = SPIFFS.open("/");
      if(dir){
        for(File e = dir.openNextFile(); e && k < CAP; e = dir.openNextFile()){
          const char* nm = e.path(); const size_t l = strlen(nm);
          if(strstr(nm, "/logo/") && l > 4 && !strcmp(nm + l - 4, ".565")) strlcpy(all[k++], nm, 32);
          e.close();
        }
        dir.close();
      }
      for(uint16_t i = 0; i < k; i++){ SPIFFS.remove(all[i]); vTaskDelay(pdMS_TO_TICKS(30)); }
      free(all);
    }
    s_noN = 0; s_noDirty = true; s_passDone = false;
    s->bump();
    Serial.printf("##LOGO#\tусі логотипи стерто (%u) — шукаю наново\n", (unsigned)k);
  }
  if(s_passDone && !s->_bForce){
    Serial.println("##LOGO#\tусі логотипи: прохід уже був — не повторюємо");
    s->_bOn = false; s->_busy = false; vTaskDelete(nullptr);
  }
  s->_bForce = false;
  LgSt* st = nullptr;
  uint16_t n = 0;
  File f = SPIFFS.open(PLAYLIST_PATH, "r");
  if(f){
    uint16_t cap = 0;
    while(f.available()){ String l = f.readStringUntil('\n'); if(l.indexOf('\t') > 0) cap++; }
    if(cap) st = (LgSt*)heap_caps_calloc(cap, sizeof(LgSt), MALLOC_CAP_SPIRAM);
    f.seek(0);
    while(st && f.available() && n < cap){
      String l = f.readStringUntil('\n');
      int t1 = l.indexOf('\t');
      if(t1 <= 0) continue;
      int t2 = l.indexOf('\t', t1 + 1);
      String u = l.substring(t1 + 1, t2 > 0 ? t2 : l.length()); u.trim();
      strlcpy(st[n].name, l.substring(0, t1).c_str(), sizeof(st[n].name));
      strlcpy(st[n].url, u.c_str(), sizeof(st[n].url));
      n++;
    }
    f.close();
  }
  s->_bN = n; s->_bI = 0;
  Serial.printf("##LOGO#\tусі логотипи: станцій у списку %u\n", (unsigned)n);
  bool complete = st != nullptr;
  for(uint16_t i = 0; st && i < n && s->_bOn; i++){
    s->_bI = i + 1;
    if(!st[i].url[0]) continue;
    const uint32_t c = crc(st[i].url);
    if(noHas(c)) continue;
    char p[28];
    snprintf(p, sizeof(p), "/logo/%08x.565", (unsigned)c);
    if(SPIFFS.exists(p)) continue;
    if(webBusy()){                              /* сторінка вантажиться — звільнити пам'ять, продовжити згодом */
      Serial.println("##LOGO#\tусі логотипи: веб-сторінка вантажиться — продовжу згодом");
      complete = false; s->_bPending = true;
      break;
    }
    /*  обкладинка проповіді просить мережу — чекаємо (до 2 хв)  */
    for(uint16_t w2 = 0; s->_hold && w2 < 600; w2++) vTaskDelay(pdMS_TO_TICKS(200));
    /*  без мережі — чекаємо до хвилини, далі прохід перервано (наступного разу — знову)  */
    uint16_t w = 0;
    while(WiFi.status() != WL_CONNECTED && w < 600){ vTaskDelay(pdMS_TO_TICKS(100)); w++; }
    if(WiFi.status() != WL_CONNECTED){ complete = false; break; }
    waitStream();
    strlcpy(s->_url, st[i].url, sizeof(s->_url));
    strlcpy(s->_name, st[i].name, sizeof(s->_name));
    const int r = s->_one();
    if(r > 0) s->_bFound++;
    else if(r < 0) s->_bNet++;                  /* мережа — цю станцію наступного увімкнення ще раз */
    vTaskDelay(pdMS_TO_TICKS(3000));            /* пауза між станціями: потік і проповіді теж ходять у мережу */
  }
  if(s->_bNet) complete = false;
  if(st) heap_caps_free(st);
  if(complete && s->_bOn) s_passDone = true;
  noSave();                                     /* один запис — і лише якщо список змінився */
  Serial.printf("##LOGO#\tусі логотипи: %s — знайдено нових %u, мережа не відповіла %u, станцій %u\n",
                complete ? "готово" : s->_bPending ? "решту — щойно звільниться веб-сторінка" : "решту — наступного увімкнення",
                (unsigned)s->_bFound, (unsigned)s->_bNet, (unsigned)n);
  s->_bOn = false;
  s->_busy = false;
  vTaskDelete(nullptr);
}

void YoLogos::fetchAll(){
  if(_bOn) return;
  if(_busy){ _bPending = true; return; }       /* іде пошук для станції, що грає, — почнемо слідом */
  _bN = config.playlistLength(); _bI = 0; _bFound = 0; _bNet = 0;
  _busy = true; _bOn = true;
  if(xTaskCreatePinnedToCore(_allTask, "logoall", 16384, this, 0, nullptr, 0) != pdPASS){
    _busy = false; _bOn = false; _bPending = true;   /* пам'яті зараз немає — спробуємо пізніше */
  }
}

/*  Кнопка «Завантажити всі логотипи» — прохід навіть якщо вже був.  */
void YoLogos::fetchAllForce(){ _bForce = true; fetchAll(); }

void YoLogos::loop(){
  static bool autoDone = false, seeded = false;
  static uint32_t pendT = 0;
  /*  Версія — ще й у адресі логотипа для сторінки (/logo/….565?v=N, кеш браузера на добу). Після
      перезапуску лік ішов знову з нуля, і браузер показував старий логотип за тією самою адресою
      («Радіо Культура» лишалась фотографією неба). Тепер початок — випадковий.  */
  if(!seeded){ seeded = true; _ver += (esp_random() & 0xFFFFF) << 8; }
  /*  раз за увімкнення (задача сама вирішить, чи прохід уже був)  */
  if(!autoDone && WiFi.status() == WL_CONNECTED && millis() > 25000){ autoDone = true; fetchAll(); }
  if(_bPending && !_busy && WiFi.status() == WL_CONNECTED && millis() - pendT > 5000 && !webBusy()){
    pendT = millis(); _bPending = false; fetchAll();
  }
}
