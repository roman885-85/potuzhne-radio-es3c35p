/*  ---------------------------------------------------------------------------
 *  Нове меню (src/m2): малювання.
 *
 *  Кадру в пам'яті тут немає. Сцена (сторінка, шапка, хвиля від дотику)
 *  малюється щоразу заново, але лише в ту смугу екрана, яку треба оновити:
 *  смуга лежить у внутрішній пам'яті й одразу йде в дисплей через DMA.
 *  Малюють у «локальних» координатах, Gfx сам зсуває їх (прокрутка, перехід
 *  між сторінками) і обрізає по смузі — тож сторінка не мусить знати, де
 *  вона зараз на екрані.
 *
 *  Усі фігури — зі згладженим краєм: край змішується з тим, що вже лежить
 *  у смузі (тло малюється першим).
 *  ------------------------------------------------------------------------- */
#ifndef m2gfx_h
#define m2gfx_h
#include <Arduino.h>
#include <Adafruit_GFX.h>
#include "m2theme.h"            /* масштаб інтерфейсу: uiP(), SW/SH логічні, PSW/PSH панелі */

namespace m2 {

struct Rect {
  int16_t x = 0, y = 0, w = 0, h = 0;
  Rect(){}
  Rect(int16_t x_, int16_t y_, int16_t w_, int16_t h_) : x(x_), y(y_), w(w_), h(h_) {}
  bool empty() const { return w <= 0 || h <= 0; }
  bool has(int16_t px, int16_t py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

enum : uint8_t { AL_L = 0, AL_C = 1, AL_R = 2 };

class Gfx {
  public:
    /*  Смуга екрана, в яку малюємо (px — внутрішня пам'ять, звичайний порядок
        байтів) — у ПІКСЕЛЯХ ПАНЕЛІ. Усе інше (origin, фігури, текст, обрізання
        через narrow/clipLocal) — у логічних пікселях сторінок; переведення —
        тут, у Gfx (uiP із m2theme.h). На ES3C28P це 1:1.  */
    void target(uint16_t* px, int16_t sx, int16_t sy, int16_t sw, int16_t sh);
    /*  Зсув «локальних» координат. Логічна частина (ox/oy) — для сторінок, що
        рахують від неї видимі рядки; у пікселі панелі зсув переводиться ОКРЕМО
        від вмісту: позиція = uiP(локальна) + uiP(зсув). Тоді на масштабі 4/3
        вміст сторінки при прокрутці рухається як одне ціле. Раніше рахувалось
        uiP(локальна + зсув), і округлення залежало від прокрутки: текст,
        картки й лінії списку на кожному кадрі розходились між собою на піксель —
        «усе смикається й рветься» (власник, 25.10.2026). На ES3C28P (1:1) —
        ті самі числа, що й раніше.  */
    void origin(int16_t ox, int16_t oy){ _ox = ox; _oy = oy; _oxP = uiP(ox); _oyP = uiP(oy); }
    /*  те саме, але зсув у пікселях панелі задано точно (плавна прокрутка й
        перехід між сторінками — на кожен піксель панелі, а не на кожен логічний)  */
    void originP(int16_t ox, int16_t oy, int16_t oxP, int16_t oyP){ _ox = ox; _oy = oy; _oxP = oxP; _oyP = oyP; }
    int16_t ox() const { return _ox; }
    int16_t oy() const { return _oy; }
    /*  обрізання — у пікселях панелі; перетинається зі смугою  */
    void clip(int16_t x, int16_t y, int16_t w, int16_t h);
    void clipLocal(int16_t x, int16_t y, int16_t w, int16_t h){
      const int16_t x0 = _px0(x), y0 = _py0(y);
      clip(x0, y0, _px0(x + w) - x0, _py0(y + h) - y0);
    }
    void unclip(){ _cx0 = _tx; _cy0 = _ty; _cx1 = _tx + _tw; _cy1 = _ty + _th; }
    /*  звузити обрізання до локального прямокутника (у межах поточного); повертає попереднє  */
    Rect narrow(int16_t x, int16_t y, int16_t w, int16_t h){
      Rect prev = clipRect();
      int16_t x0 = _px0(x), y0 = _py0(y), x1 = _px0(x + w), y1 = _py0(y + h);
      if(x0 < _cx0) x0 = _cx0; if(y0 < _cy0) y0 = _cy0;
      if(x1 > _cx1) x1 = _cx1; if(y1 > _cy1) y1 = _cy1;
      if(x1 < x0) x1 = x0; if(y1 < y0) y1 = y0;
      _cx0 = x0; _cy0 = y0; _cx1 = x1; _cy1 = y1;
      return prev;
    }
    /*  те саме в пікселях панелі: межа між сторінками під час переходу  */
    Rect narrowP(int16_t x0, int16_t y0, int16_t x1, int16_t y1){
      Rect prev = clipRect();
      if(x0 < _cx0) x0 = _cx0; if(y0 < _cy0) y0 = _cy0;
      if(x1 > _cx1) x1 = _cx1; if(y1 > _cy1) y1 = _cy1;
      if(x1 < x0) x1 = x0; if(y1 < y0) y1 = y0;
      _cx0 = x0; _cy0 = y0; _cx1 = x1; _cy1 = y1;
      return prev;
    }
    void restore(const Rect& r){ _cx0 = r.x; _cy0 = r.y; _cx1 = r.x + r.w; _cy1 = r.y + r.h; }
    Rect clipRect() const { return Rect(_cx0, _cy0, _cx1 - _cx0, _cy1 - _cy0); }
    /*  чи видно хоч щось із прямокутника (локальні координати)  */
    bool visible(int16_t x, int16_t y, int16_t w, int16_t h) const {
      const int16_t sx = _px0(x), sy = _py0(y);
      return sx < _cx1 && sy < _cy1 && _px0(x + w) > _cx0 && _py0(y + h) > _cy0;
    }

    void fill(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c);
    void fillA(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c, uint8_t a);   /* напівпрозоро */
    /*  тло з ледь помітним переходом кольору згори вниз (з розсіюванням, без смуг)  */
    void vgrad(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c0, uint16_t c1);
    void box(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t r, uint16_t c);
    void frame(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t r, uint16_t c, uint8_t t = 1);
    void circle(float cx, float cy, float r, uint16_t c);
    void ring(float cx, float cy, float r, float wd, uint16_t c){ arc(cx, cy, r, wd, c); }
    void line(float x0, float y0, float x1, float y1, float wd, uint16_t c);
    void arc(float cx, float cy, float r, float wd, uint16_t c, float a0 = 0, float a1 = 360);   /* 0 — вгору, за годинниковою */
    void poly(const float* xy, uint8_t n, uint16_t c);
    /*  Хвиля світла: коло (cx,cy,rad) усередині заокругленого прямокутника area, прозорість a.  */
    void lighten(const Rect& area, uint8_t r, float cx, float cy, float rad, uint8_t a, uint16_t c = 0xFFFF);
    /*  картинка RGB565 (звичайний порядок байтів) з заокругленими кутами  */
    void image(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t* px, uint8_t r = 0);
    /*  готовий прямокутник пікселів як є — без змішування й кутів (кеш рядків списку).
        w×h — пікселі ПАНЕЛІ (буфер, намальований Gfx у пікселях панелі); x, y — логічні.  */
    void blit(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t* px);

    /*  Текст UTF-8 згладженим шрифтом (fonts/aa). Повертає ширину (логічну).
        maxw > 0 — не ширше: обрізає з «…». На ES3C35P шрифти вже в пікселях
        панелі (×4/3) — ширина ділиться назад на масштаб.  */
    int16_t text(int16_t x, int16_t baseline, const char* utf8, const GFXfont* f, uint16_t c,
                 uint8_t align = AL_L, int16_t maxw = 0);
    static int16_t textW(const char* utf8, const GFXfont* f);

    static uint16_t blend(uint16_t bg, uint16_t fg, uint8_t a);
    static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b){ return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }

  private:
    uint16_t* _px = nullptr;
    int16_t _tx = 0, _ty = 0, _tw = 0, _th = 0;
    int16_t _cx0 = 0, _cy0 = 0, _cx1 = 0, _cy1 = 0;
    int16_t _ox = 0, _oy = 0;
    int16_t _oxP = 0, _oyP = 0;           /* той самий зсув у пікселях панелі */
    /*  локальні логічні → пікселі панелі: вміст і зсув — окремо (див. origin)  */
    inline int16_t _px0(int32_t v) const { return uiP(v) + _oxP; }
    inline int16_t _py0(int32_t v) const { return uiP(v) + _oyP; }
    inline float   _pxf(float v) const { return uiPf(v) + _oxP; }
    inline float   _pyf(float v) const { return uiPf(v) + _oyP; }
    void _fillP(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t c);   /* прямокутник у пікселях панелі */
    void _boxP(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t r, uint16_t c);
    inline void _cover(int16_t sx, int16_t sy, float cov, uint16_t c){
      if(cov <= 0 || sx < _cx0 || sy < _cy0 || sx >= _cx1 || sy >= _cy1) return;
      uint16_t* p = _px + (int32_t)(sy - _ty) * _tw + (sx - _tx);
      *p = cov >= 1 ? c : blend(*p, c, (uint8_t)(cov * 255));
    }
    void _glyphs(int16_t sx, int16_t sb, const uint8_t* cp, uint16_t n, const GFXfont* f, uint16_t c);   /* пікселі панелі */
};

/*  UTF-8 → CP1251 (як у шрифтах): out має вміщати len+1.  */
uint16_t toCp1251(const char* utf8, uint8_t* out, uint16_t cap);

}  // namespace m2
#endif
