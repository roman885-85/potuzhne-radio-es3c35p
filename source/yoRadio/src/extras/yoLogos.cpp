#include "yoLogos.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include "miniz.h"
#include "esp_timer.h"
#include "yoSermons.h"          /* yoJpegFit */
#include "../core/options.h"
#include "../core/config.h"      /* PLAYLIST_PATH */

YoLogos logos;

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
  if(x1 < x0 || y1 < y0){ cx0 = cy0 = 0; cw = w; ch = h; return; }
  const uint32_t pad = ((x1 - x0) > (y1 - y0) ? (x1 - x0) : (y1 - y0)) * 6 / 100 + 1;
  cx0 = x0 > pad ? x0 - pad : 0; cy0 = y0 > pad ? y0 - pad : 0;
  const uint32_t ex = x1 + pad + 1 < w ? x1 + pad + 1 : w, ey = y1 + pad + 1 < h ? y1 + pad + 1 : h;
  cw = ex - cx0; ch = ey - cy0;
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
  uint16_t fill = 0xFFFF;                         /* поля навколо вписаного — білі (прозоре теж на білому) */
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
  }else cropRect(w, h, crop, cx0, cy0, cw, chh);
  /*  область джерела і куди вона лягає в W×H (з полями, як fitRGBA)  */
  const float f = fminf((float)W / cw, (float)H / chh);
  int dw = (int)(cw * f), dh = (int)(chh * f); if(dw < 1) dw = 1; if(dh < 1) dh = 1;
  const int ox = (W - dw) / 2, oy = (H - dh) / 2;
  uint32_t* acc = (uint32_t*)heap_caps_calloc((size_t)dw * dh * 4, 4, MALLOC_CAP_SPIRAM);
  uint16_t* xmap = (uint16_t*)ps_malloc((size_t)w * 2);
  bool ok = false;
  if(acc && xmap){
    for(uint32_t x = 0; x < w; x++) xmap[x] = (x < cx0 || x >= cx0 + cw) ? 0xFFFF : (uint16_t)((uint64_t)(x - cx0) * dw / cw);
    ok = pngRows(d, n, *pi, [&](uint32_t y, const uint8_t* row){
      if(y < cy0 || y >= cy0 + chh) return;
      const uint32_t py = (uint64_t)(y - cy0) * dh / chh;
      for(uint32_t x = 0; x < w; x++){
        if(xmap[x] == 0xFFFF) continue;
        const uint8_t* q = row + x * 4; const uint32_t al = q[3];
        uint32_t* a4 = acc + ((size_t)py * dw + xmap[x]) * 4;
        if(al == 255){ a4[0] += q[0]; a4[1] += q[1]; a4[2] += q[2]; }
        else{ a4[0] += (q[0] * al + 255 * (255 - al)) / 255; a4[1] += (q[1] * al + 255 * (255 - al)) / 255; a4[2] += (q[2] * al + 255 * (255 - al)) / 255; }
        a4[3]++;
      }
    });
    if(ok){
      for(int yy = 0; yy < H; yy++)
        for(int xx = 0; xx < W; xx++){
          uint16_t pxl = fill;
          const int sx = xx - ox, sy = yy - oy;
          if(sx >= 0 && sy >= 0 && sx < dw && sy < dh){
            const uint32_t* a4 = acc + ((size_t)sy * dw + sx) * 4;
            if(a4[3]){ uint32_t r = a4[0] / a4[3], g = a4[1] / a4[3], b = a4[2] / a4[3]; pxl = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
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

/*  RGBA -> W x H RGB565, прозоре на білому, з усередненням пікселів.  */
static void fitRGBA(const uint8_t* rgba, uint32_t sw0, uint32_t sh0, uint16_t* out, int W, int H, float crop = 0){
  uint32_t cx0, cy0, sw, sh;
  uint16_t fill = 0xFFFF;
  if(crop < 0) yoContentBox(rgba, sw0, sh0, 4, cx0, cy0, sw, sh, &fill);   /* банер — лише сам логотип, поля — його фоном */
  else cropRect(sw0, sh0, crop, cx0, cy0, sw, sh);
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
        uint32_t R = 0, G = 0, B = 0, k = 0;
        for(int yy = y0; yy < y1; yy++)
          for(int xx = x0; xx < x1; xx++){
            const uint8_t* p = rgba + ((size_t)(yy + cy0) * sw0 + xx + cx0) * 4;
            uint32_t a = p[3];
            R += (p[0] * a + 255 * (255 - a)) / 255; G += (p[1] * a + 255 * (255 - a)) / 255; B += (p[2] * a + 255 * (255 - a)) / 255; k++;
          }
        r = R / k; g = G / k; b = B / k;
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

/*  Одна спроба GET у буфер PSRAM. Довжина; 0 — сервер відповів «нема» (4xx);
    -1 — мережа або сервер тимчасово не впорався (5xx, 429) — варто повторити.  */
static int httpGetOnce(const char* url0, uint8_t* buf, size_t cap){
  /*  Переадресації — самі: HTTPClient лишається на тому ж типі з'єднання, і з
      http:// на https:// (301 radiomaria.org.ua, sw-radio.com) не переходив —
      логотип вважався «нема».  */
  char url[480];
  strlcpy(url, url0, sizeof(url));
  for(uint8_t hop = 0; hop < 5; hop++){
    const bool tls = !strncmp(url, "https", 5);
    WiFiClient plain; WiFiClientSecure sec; sec.setInsecure();
    HTTPClient http;
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
    size_t len = 0;
    /*  великий значок — довше: 10 с + секунда на кожні 20 КБ (PNG 1 МБ через TLS іде ~30–50 с)  */
    uint32_t deadline = millis() + 10000 + (total > 0 ? (uint32_t)total / 20 : 0);
    while(len < cap && (int32_t)(deadline - millis()) > 0 && (total < 0 || (int)len < total)){
      int av = st->available();
      if(!av){ if(!st->connected()) break; vTaskDelay(pdMS_TO_TICKS(5)); continue; }
      len += st->read(buf + len, (size_t)av > cap - len ? cap - len : av);
    }
    http.end();
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

static int httpGet(const char* url, uint8_t* buf, size_t cap){
  struct NetMark { NetMark(){ s_inNet = true; } ~NetMark(){ s_inNet = false; } } mark;
  for(uint8_t a = 0; a < 6; a++){
    if(a){ s_tries++; vTaskDelay(pdMS_TO_TICKS(1500)); }
    int n = httpGetOnce(url, buf, cap);
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
      if(strcasestr(u, ".svg") || strcasestr(u, ".webp") || !strncmp(u, "data:", 5)) continue;
      absUrl(base, u, out[k].url, sizeof(out[k].url)); out[k].size = sz; out[k].og = false; k++;
    }else{
      if(!(tagAttr(p, te, "property", a, sizeof(a)) || tagAttr(p, te, "name", a, sizeof(a))) || strcasecmp(a, "og:image")) continue;
      if(!tagAttr(p, te, "content", u, sizeof(u)) || strcasestr(u, ".svg") || strcasestr(u, ".webp")) continue;
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
    якщо попередній значок не розпакувався).  */
static bool pickFavicon(const char* json, const char* myName, const char* myUrl, uint8_t mode, uint8_t skip, char* out, size_t cap){
  char nm[100], my[100], fav[256], cc[4], u1[200], h1[64], h2[64], myHost[64], myCc[3];
  normName(myName, my, sizeof(my));
  hostOf(myUrl, myHost, sizeof(myHost));
  const bool hasCc = tldCountry(myHost, myCc);
  for(uint8_t pass = 0; pass < (mode ? 3 : 1); pass++){
    uint8_t seen = 0;
    const char* p = json;
    while((p = strchr(p, '{')) != nullptr){
      const char* e = strchr(p, '}');
      if(!e) break;
      if(jsonField(p, e, "favicon", fav, sizeof(fav)) && !strncmp(fav, "http", 4) && !strstr(fav, ".svg")){
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
  if(_busy || WiFi.status() != WL_CONNECTED) return false;
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
  /*  Імена — у PSRAM: кнопка «Шукати знову» виконується в головному циклі, а там
      вільного стека ~3,7 КБ — 2 КБ масиву на ньому було забагато.  */
  const uint16_t CAP = 200;
  char (*names)[32] = (char (*)[32])ps_malloc((size_t)CAP * 32);
  if(!names) return 0;
  uint16_t n = 0, k = 0;
  File dir = SPIFFS.open("/");
  if(dir){
    for(File f = dir.openNextFile(); f && k < CAP; f = dir.openNextFile()){
      const char* nm = f.path();
      if(strstr(nm, "/logo/") && strstr(nm, ".no") && !strstr(nm, "all")) strlcpy(names[k++], nm, 32);
      f.close();
    }
    dir.close();
  }
  for(uint16_t i = 0; i < k; i++){ SPIFFS.remove(names[i]); n++; }
  free(names);
  return n;
}

/*  Один пошук для _url/_name (з задачі): у SPIFFS — .565 (є) або .no (нема).
    Повертає 1 — знайдено, 0 — нема, -1 — мережа.  */
int YoLogos::_one(){
  s_tries = 0;
  _crc = crc(_url);
  uint8_t* buf = (uint8_t*)ps_malloc(LG_MAXDL + 1);
  uint8_t* js = (uint8_t*)ps_malloc(64 * 1024 + 1);
  char fav[256] = {0};
  int result = -1;                            /* -1 мережа, 0 нема логотипа, 1 є */
  if(buf && js){
    char q[460], enc[400];
    int n = 0, jl = -1;
    uint8_t mode = 0;
    /*  1) той самий потік (як записано, і з http↔https)  */
    for(uint8_t v = 0; v < 2 && jl != -2; v++){
      char u2[210];
      if(v == 0) strlcpy(u2, _url, sizeof(u2));
      else if(!strncmp(_url, "http://", 7)){ snprintf(u2, sizeof(u2), "https://%s", _url + 7); }
      else if(!strncmp(_url, "https://", 8)){ snprintf(u2, sizeof(u2), "http://%s", _url + 8); }
      else break;
      urlEnc(u2, enc, sizeof(enc));
      snprintf(q, sizeof(q), LG_API "byurl?url=%s", enc);
      n = httpGet(q, js, 64 * 1024);
      if(n < 0){ jl = -2; break; }
      js[n] = 0;
      if(pickFavicon((char*)js, _name, _url, 0, 0, fav, sizeof(fav))){ jl = n; mode = 0; break; }
    }
    /*  2) пошук за назвою: та сама назва (та країна) або той самий сервер потоку  */
    if(jl == -1){
      urlEnc(_name, enc, sizeof(enc));
      snprintf(q, sizeof(q), LG_API "search?name=%s&limit=40&hidebroken=true", enc);
      n = httpGet(q, js, 64 * 1024);
      if(n < 0) jl = -2;
      else{ js[n] = 0; if(pickFavicon((char*)js, _name, _url, 1, 0, fav, sizeof(fav))){ jl = n; mode = 1; } }
    }
    if(jl == -2) result = -1;
    if(jl >= 0){
      /*  значок з каталогу; не розпакувався (SVG під іншим ім'ям, битий файл) — наступний кандидат  */
      result = 0;
      for(uint8_t k = 0; k < 3 && result == 0; k++){
        if(k && !pickFavicon((char*)js, _name, _url, mode, k, fav, sizeof(fav))) break;
        /*  Wikimedia віддає зменшені копії лише стандартних ширин (798px — помилка 400):
            «…/thumb/…/798px-Файл.png» → «…/250px-Файл.png»  */
        if(strstr(fav, "upload.wikimedia.org") && strstr(fav, "/thumb/")){
          char* seg = strrchr(fav, '/');
          char* px = seg ? strstr(seg, "px-") : nullptr;
          if(px && px > seg + 1){
            bool digits = true; for(char* q = seg + 1; q < px; q++) if(!isdigit((unsigned char)*q)) digits = false;
            if(digits){ char tail[200]; strlcpy(tail, px, sizeof(tail)); snprintf(seg + 1, sizeof(fav) - (seg + 1 - fav), "250%s", tail); }
          }
        }
        n = httpGet(fav, buf, LG_MAXDL);
        if(n < 0){ result = -1; break; }
        uint16_t* px = (uint16_t*)ps_malloc(LOGO_S * LOGO_S * 2);
        if(px && n > 0 && yoImageFit(buf, n, px, LOGO_S, LOGO_S)){
          char path[28]; snprintf(path, sizeof(path), "/logo/%08x.565", (unsigned)_crc);
          File o = SPIFFS.open(path, "w");
          if(o){ o.write((uint8_t*)px, LOGO_S * LOGO_S * 2); o.close(); result = 1; }
        }
        if(px) free(px);
      }
    }
    /*  у каталозі немає або його значки не відкрились — з сайту станції  */
    if(jl == -1 || (jl >= 0 && result == 0)){
      /*  3) у каталозі немає — значок із сайту станції (головна сторінка: <link>, og:image; далі служба Google)  */
      result = 0;
      char host[64], site[64], label[40];
      hostOf(_url, host, sizeof(host));
      siteOf(host, site, sizeof(site), label, sizeof(label));
      if(site[0] && siteMatches(label, _name)){
        /*  головна сторінка сайту: значки з <link> і og:image  */
        SiteIcon ic[10]; uint8_t ni = 0;
        char base[96];
        for(uint8_t v = 0; v < 2 && !ni; v++){
          snprintf(base, sizeof(base), v ? "https://www.%s" : "https://%s", site);
          char home[110]; snprintf(home, sizeof(home), "%s/", base);
          n = httpGet(home, buf, 512 * 1024);
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
        order[no++] = -1;                                   /* служба Google */
        for(uint8_t k = 0; k < no && k < 6 && result == 0; k++){
          char cu[220]; float crop = 0;
          if(order[k] < 0) snprintf(cu, sizeof(cu), "https://www.google.com/s2/favicons?domain=%s&sz=128", site);
          else{ strlcpy(cu, ic[order[k]].url, sizeof(cu)); if(ic[order[k]].og) crop = -1; }   /* банер — межі логотипа */
          n = httpGet(cu, buf, LG_MAXDL);
          if(n < 0){ result = -1; break; }
          uint16_t* px = (uint16_t*)ps_malloc(LOGO_S * LOGO_S * 2);
          if(px && n > 64 && yoImageFit(buf, n, px, LOGO_S, LOGO_S, crop)){
            char path[28]; snprintf(path, sizeof(path), "/logo/%08x.565", (unsigned)_crc);
            File o = SPIFFS.open(path, "w");
            if(o){ o.write((uint8_t*)px, LOGO_S * LOGO_S * 2); o.close(); result = 1; strlcpy(fav, cu, sizeof(fav)); }
          }
          if(px) free(px);
        }
      }
    }
  }
  if(buf) free(buf);
  if(js) free(js);
  if(result == 0){                             /* нема — не питати щоразу */
    char path[28]; snprintf(path, sizeof(path), "/logo/%08x.no", (unsigned)_crc);
    File o = SPIFFS.open(path, "w"); if(o){ o.print("1"); o.close(); }
  }
  Serial.printf("##LOGO#\t%s: %s %s (повторів %u)\n", _name, result > 0 ? "знайдено" : result == 0 ? "нема" : "мережа", fav, s_tries);
  if(result > 0) _ver++;
  return result;
}

void YoLogos::_task(void* arg){
  YoLogos* s = (YoLogos*)arg;
  if(!s->_quick) vTaskDelay(pdMS_TO_TICKS(3000));   /* нехай станція спершу підключиться */
  s->_quick = false;
  s->_one();
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
  uint8_t* buf = (uint8_t*)ps_malloc(LG_MAXDL + 1);
  uint16_t* px = (uint16_t*)ps_malloc(LOGO_S * LOGO_S * 2);
  int n = buf ? httpGet(s->_tu, buf, LG_MAXDL) : -1;
  const int64_t t0 = esp_timer_get_time();
  bool ok = n > 0 && px && yoImageFit(buf, n, px, LOGO_S, LOGO_S);
  const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
  Serial.printf("LOGOTEST %s\n  байтів %d (повторів %u), підпис %02X %02X %02X %02X, розпаковано: %s за %u мкс\n",
                s->_tu, n, s_tries, n > 3 ? buf[0] : 0, n > 3 ? buf[1] : 0, n > 3 ? buf[2] : 0, n > 3 ? buf[3] : 0,
                ok ? "так" : "НІ", (unsigned)us);
  if(buf) free(buf);
  if(px) free(px);
  s->_busy = false;
  vTaskDelete(nullptr);
}

/*  ---------- усі станції списку ---------- */
/*  «all3»: прохід із розумнішим пошуком, переадресаціями й stb_image (25.10.2026) — старі
    позначки «нема» один раз скидаються, і всі станції без логотипа шукаються наново.  */
#define LG_ALLFLAG "/logo/all7.ok"

/*  Список «назва<Tab>адреса<Tab>гучність» — у PSRAM, один раз за прохід.  */
struct LgSt { char name[64]; char url[200]; };

/*  Окрема задача на весь прохід: головний цикл (дотик, звук) файлів не читає.  */
void YoLogos::_allTask(void* arg){
  YoLogos* s = (YoLogos*)arg;
  if(s->_bForget){
    const uint16_t k = s->forget();
    File o = SPIFFS.open("/logo/all7.no", "w"); if(o){ o.print("1"); o.close(); }
    s->_bForget = false;
    Serial.printf("##LOGO#\tпозначок «нема» скинуто: %u\n", (unsigned)k);
  }
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
    char p[28];
    snprintf(p, sizeof(p), "/logo/%08x.565", (unsigned)c);
    if(SPIFFS.exists(p)) continue;
    snprintf(p, sizeof(p), "/logo/%08x.no", (unsigned)c);
    if(SPIFFS.exists(p)) continue;
    /*  обкладинка проповіді просить мережу — чекаємо (до 2 хв)  */
    for(uint16_t w2 = 0; s->_hold && w2 < 600; w2++) vTaskDelay(pdMS_TO_TICKS(200));
    /*  без мережі — чекаємо до хвилини, далі прохід перервано (наступного разу — знову)  */
    uint16_t w = 0;
    while(WiFi.status() != WL_CONNECTED && w < 600){ vTaskDelay(pdMS_TO_TICKS(100)); w++; }
    if(WiFi.status() != WL_CONNECTED){ complete = false; break; }
    strlcpy(s->_url, st[i].url, sizeof(s->_url));
    strlcpy(s->_name, st[i].name, sizeof(s->_name));
    const int r = s->_one();
    if(r > 0) s->_bFound++;
    else if(r < 0) s->_bNet++;                  /* мережа — цю станцію наступного проходу ще раз */
    vTaskDelay(pdMS_TO_TICKS(1500));            /* пауза між станціями: проповіді теж ходять у мережу */
  }
  if(s->_bNet) complete = false;
  if(st) heap_caps_free(st);
  if(complete && s->_bOn){
    File o = SPIFFS.open(LG_ALLFLAG, "w");
    if(o){ o.print("1"); o.close(); }
  }
  Serial.printf("##LOGO#\tусі логотипи: %s — знайдено нових %u, мережа не відповіла %u, станцій %u\n",
                complete ? "готово" : "ще раз за 10 хв", (unsigned)s->_bFound, (unsigned)s->_bNet, (unsigned)n);
  s->_bAgainAt = complete ? 0 : (millis() + 600000UL) | 1;
  s->_bOn = false;
  s->_busy = false;
  vTaskDelete(nullptr);
}

void YoLogos::fetchAll(){
  if(_bOn) return;
  if(_busy){ _bPending = true; return; }       /* іде пошук для станції, що грає, — почнемо слідом */
  _bN = config.playlistLength(); _bI = 0; _bFound = 0; _bNet = 0; _bAgainAt = 0;
  _busy = true; _bOn = true;
  if(xTaskCreatePinnedToCore(_allTask, "logoall", 16384, this, 0, nullptr, 0) != pdPASS){
    _busy = false; _bOn = false; _bPending = true;   /* пам'яті зараз немає — спробуємо пізніше */
  }
}

void YoLogos::loop(){
  static bool autoDone = false;
  static uint32_t pendT = 0;
  if(!autoDone && WiFi.status() == WL_CONNECTED && millis() > 25000){
    autoDone = true;
    if(!SPIFFS.exists(LG_ALLFLAG)){                /* разово: позначка — після повного проходу без мережевих збоїв */
      _bForget = !SPIFFS.exists("/logo/all7.no");  /* перший прохід розумнішого пошуку: позначки «нема» з минулого — геть (у задачі) */
      fetchAll();
    }
  }
  if(_bAgainAt && !_bOn && (int32_t)(millis() - _bAgainAt) >= 0 && WiFi.status() == WL_CONNECTED){ _bAgainAt = 0; fetchAll(); }
  if(_bPending && !_busy && WiFi.status() == WL_CONNECTED && millis() - pendT > 5000){
    pendT = millis(); _bPending = false; fetchAll();
  }
}
