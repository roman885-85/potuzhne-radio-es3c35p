#include "yoSplash.h"
#include <LittleFS.h>
#include "esp_heap_caps.h"
#include "../core/options.h"
#include "../displays/dspcore.h"
#include "../displays/tools/spidma.h"
#include "../displays/tools/lcdout.h"
#include "yoSfx.h"

extern DspCore dsp;
YoSplash splash;

static const uint32_t PX_CAP = 320 * 16;   /* найбільша смуга: ширина × висота квадрата (8), із запасом */
/*  Кадри заставки — не більші за екран; ставимо вгорі посередині (на ES3C28P —
    320×156 з (0,0); на ES3C35P — 480×208, tools/make_splash.py --large).  */

bool YoSplash::begin(bool sync, const char* path){
  stop();
  if(!YoSfx::mounted()){ Serial.println("##DSP#\tзаставка: розділ ресурсів не змонтовано"); return false; }
  File f = LittleFS.open(path, "r");
  if(!f){ Serial.printf("##DSP#\tзаставка: немає %s\n", path); return false; }
  size_t sz = f.size();
  if(sz < 16 || sz > 3 * 1024 * 1024){ f.close(); return false; }
  _data = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if(!_data){ f.close(); return false; }
  size_t got = f.read(_data, sz);
  f.close();
  if(got != sz || memcmp(_data, "YAN1", 4)){ stop(); return false; }
  auto u16 = [&](size_t o){ return (uint16_t)(_data[o] | (_data[o + 1] << 8)); };
  _w = u16(4); _h = u16(6); _n = u16(8); _fps = u16(10); _loop = u16(12); _wrap = u16(14);
  if(!_n || !_w || !_h || _w > DSP_W || _h > DSP_H || !_fps || _loop >= _n || _wrap >= _n){ stop(); return false; }
  _offs = (uint32_t*)heap_caps_malloc(_n * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
  if(!_offs){ stop(); return false; }
  size_t o = 16;
  for(uint16_t i = 0; i < _n; i++){
    if(o + 4 > sz){ stop(); return false; }
    uint32_t ln = _data[o] | (_data[o+1] << 8) | (_data[o+2] << 16) | ((uint32_t)_data[o+3] << 24);
    _offs[i] = o + 4;
    o += 4 + ln;
  }
  if(o > sz){ stop(); return false; }
  _size = sz;
  _px = (uint8_t*)lcdScratch(PX_CAP * 2);               /* спільна смуга (spidma.h) */
  if(!_px){ stop(); return false; }
#if LCD_ROW_ALIGN > 1
  /*  ST77922 бере смуги лише цілими рядками, кратними 8 (стиснення блоками), а
      латки заставки — довільні. Тож латки лягають на полотно в PSRAM, а в
      панель іде охоплена ними смуга, вирівняна на 8 рядків.  */
  /*  ширина полотна — кратна 8 (поворот смуг блоками 8×8)  */
  _cw = (_w + 7) & ~7;
  _cv = (uint16_t*)heap_caps_aligned_alloc(16, (size_t)_cw * ((_h + 7) & ~7) * 2, MALLOC_CAP_SPIRAM);
  if(!_cv){ stop(); return false; }
  memset(_cv, 0, (size_t)_cw * ((_h + 7) & ~7) * 2);
#endif
  _cur = 0; _nextMs = millis();
  _hold = sync; _holdSince = millis(); _firstMs = 0;
  return true;
}

bool YoSplash::worthWaiting() const {
  if(!_data || introDone()) return false;
  const uint32_t now = millis();
  if(!_firstMs) return now < 12000;                     /* ще чекає звуку ввімкнення */
  const uint32_t intro = (uint32_t)_loop * 1000 / (_fps ? _fps : 25);
  return now - _firstMs < (intro + 600 < 4000 ? intro + 600 : 4000);
}

void YoSplash::stop(){
  if(_data){ heap_caps_free(_data); _data = nullptr; }
  if(_offs){ heap_caps_free(_offs); _offs = nullptr; }
  if(_cv){ heap_caps_free(_cv); _cv = nullptr; }
  _px = nullptr;                                          /* смуга спільна — не звільняємо */
  _size = 0; _n = 0; _cur = 0;
}

void YoSplash::tick(){
  if(!_data) return;
  uint32_t now = millis();
  if(_hold){
    /*  Чекаємо, доки звук увімкнення справді зазвучить (підсилювач прокидається
        ~0,4 с), щоб точка засвітилась разом зі «зловленою станцією». Звуку не
        буде (вимкнено) — починаємо одразу; довше 3 с не чекаємо ніколи.  */
    bool heard = sfx.audibleMs(SFX_START) && sfx.audibleMs(SFX_START) >= _holdSince;
    bool none  = sfx.ready() && !sfx.willPlay(SFX_START) && now - _holdSince > 300;
    if(!heard && !none && now - _holdSince < 3000) return;
    _hold = false;
    _nextMs = now;
  }
  if(!_firstMs){ _firstMs = now ? now : 1; Serial.printf("##DSP#\tзаставка: перший кадр на %u мс від увімкнення\n", (unsigned)now); }
  if((int32_t)(now - _nextMs) < 0) return;
  uint16_t idx;
  if(_cur < _wrap) idx = _cur;              /* звичайний кадр */
  else idx = _wrap;                         /* кінець петлі → її початок */
  _draw(idx);
  if(_cur < _wrap - 1) _cur++;
  else if(_cur == _wrap - 1) _cur = _wrap;   /* далі — перехідний кадр */
  else _cur = _loop + 1;                     /* після переходу — другий кадр петлі */
  /*  тримаємо темп, але не доганяємо пропущене ривком  */
  _nextMs += 1000 / _fps;
  if((int32_t)(now - _nextMs) > 200) _nextMs = now + 1000 / _fps;
}

void YoSplash::_draw(uint16_t idx){
  const uint8_t* p = _data + _offs[idx];
  const uint8_t* end = _data + _size;
#if LCD_ROW_ALIGN > 1
  if(_cv){
    /*  розпакувати латки кадру на полотно (байти у файлі — у порядку дисплея)  */
    int16_t y0 = _h, y1 = 0;
    while(p + 8 <= end){
      uint16_t x = p[0] | (p[1] << 8), y = p[2] | (p[3] << 8), w = p[4] | (p[5] << 8), h = p[6] | (p[7] << 8);
      p += 8;
      if(!w) break;
      uint32_t n = (uint32_t)w * h;
      if(x + w > _w || y + h > _h) break;
      uint32_t k = 0;
      while(k < n && p < end){
        uint8_t t = *p++;
        if(t < 128){
          uint32_t c = t + 1;
          if(k + c > n || p + c * 2 > end){ k = n; break; }
          for(uint32_t j = 0; j < c; j++, k++){ _cv[(int32_t)(y + k / w) * _cw + x + k % w] = (uint16_t)((p[0] << 8) | p[1]); p += 2; }
        }else{
          uint32_t c = t - 126;
          if(k + c > n || p + 2 > end){ k = n; break; }
          const uint16_t v = (uint16_t)((p[0] << 8) | p[1]);
          p += 2;
          for(uint32_t j = 0; j < c; j++, k++) _cv[(int32_t)(y + k / w) * _cw + x + k % w] = v;
        }
      }
      if(y < y0) y0 = y;
      if(y + h > y1) y1 = y + h;
    }
    if(y1 <= y0) return;
    y0 &= ~7; y1 = (y1 + 7) & ~7;
    const int16_t sx = ((DSP_W - _cw) / 2) & ~7;
    lcdBegin();
    lcdPush(sx, y0, _cw, y1 - y0, _cv + (int32_t)y0 * _cw);
    lcdEnd();
    return;
  }
#endif
#if DSP_MODEL==DSP_ILI9341
  bool dma = spidmaOk() || spidmaBegin();
  dsp.startWrite();
  while(p + 8 <= end){
    uint16_t x = p[0] | (p[1] << 8), y = p[2] | (p[3] << 8), w = p[4] | (p[5] << 8), h = p[6] | (p[7] << 8);
    p += 8;
    if(!w) break;
    uint32_t n = (uint32_t)w * h;
    if(n > PX_CAP || x + w > _w || y + h > _h) break;   /* зіпсований файл — далі не малюємо */
    uint32_t k = 0;
    while(k < n && p < end){
      uint8_t t = *p++;
      if(t < 128){
        uint32_t c = t + 1;
        if(k + c > n || p + c * 2 > end) { k = n; break; }
        memcpy(_px + k * 2, p, c * 2);
        p += c * 2; k += c;
      }else{
        uint32_t c = t - 126;
        if(k + c > n || p + 2 > end) { k = n; break; }
        uint8_t hi = p[0], lo = p[1];
        p += 2;
        for(uint32_t j = 0; j < c; j++){ _px[(k + j) * 2] = hi; _px[(k + j) * 2 + 1] = lo; }
        k += c;
      }
    }
    dsp.setAddrWindow(x, y, w, h);
    if(!(dma && spidmaWrite(_px, n * 2))) dsp.writePixels((uint16_t*)_px, n, true, true);
  }
  dsp.endWrite();
#endif
}
