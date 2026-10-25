#include "options.h"
#if (TS_MODEL!=TS_MODEL_UNDEFINED) && (DSP_MODEL!=DSP_DUMMY)
#include "Arduino.h"
#include "touchscreen.h"
#include "../m2/m2player.h"
#include "../m2/m2ui.h"
#include "config.h"
#include "display.h"
#include "network.h"
#include "../menu/yoMenu.h"
#include "../extras/yoSfx.h"
#include "../extras/yoExtras.h"

#ifndef TS_FT6336_ADDR
  #define TS_FT6336_ADDR        0x38
#endif
#ifndef TS_X_MIN
  #define TS_X_MIN              400
#endif
#ifndef TS_X_MAX
  #define TS_X_MAX              3800
#endif
#ifndef TS_Y_MIN
  #define TS_Y_MIN              260
#endif
#ifndef TS_Y_MAX
  #define TS_Y_MAX              3800
#endif

#if TS_MODEL==TS_MODEL_XPT2046
  #ifdef TS_SPIPINS
    SPIClass  TSSPI(HSPI);
  #endif
  #include <XPT2046_Touchscreen.h>
  XPT2046_Touchscreen ts(TS_CS);
#elif TS_MODEL==TS_MODEL_FT6336
  #include "../FT6336_Touchscreen/yoFT6336.h"
  YoFT6336 ts = YoFT6336(TS_SDA, TS_SCL, TS_INT, TS_RST);
#elif TS_MODEL==TS_MODEL_ST77922
  #include "../ST77922_Touch/yoST77922Touch.h"
  YoST77922Touch ts = YoST77922Touch(TS_SDA, TS_SCL, TS_INT, TS_RST);
#endif
/*  FT6336 і сенсор ST77922 читаються однаково: read(), isTouched, points[], rawX/rawY  */
#if TS_MODEL==TS_MODEL_FT6336 || TS_MODEL==TS_MODEL_ST77922
  #define TS_I2C_PANEL 1
#else
  #define TS_I2C_PANEL 0
#endif

void TouchScreen::init(uint16_t w, uint16_t h){
#if TS_MODEL==TS_MODEL_XPT2046
  #ifdef TS_SPIPINS
    TSSPI.begin(TS_SPIPINS);
    ts.begin(TSSPI);
  #else
    ts.begin();
  #endif
#elif TS_MODEL==TS_MODEL_FT6336
  /*  Тачскрину — той самий номер повороту, що й дисплею: він сам переведе
      портретні координати панелі в екранні.  */
  ts.begin(TS_FT6336_ADDR);
#elif TS_MODEL==TS_MODEL_ST77922
  ts.begin(0x55);
#endif
  _width  = w;
  _height = h;
  flip();
#if TS_I2C_PANEL
  ts.setResolution(_width, _height);
#endif
}

void TouchScreen::flip(){
#if TS_MODEL==TS_MODEL_XPT2046
  ts.setRotation(config.store.fliptouch?3:1);
#elif TS_MODEL==TS_MODEL_FT6336
  ts.setRotation(config.store.fliptouch?1:3);
#elif TS_MODEL==TS_MODEL_ST77922
  /*  ES3C35P: звичайне положення — поворот 1 (USB праворуч), як і в картинки  */
  ts.setRotation(config.store.fliptouch?3:1);
#endif
}

void TouchScreen::_applyCalib(uint16_t& x, uint16_t& y){
  const ExtStore& s = extras.s;
  const int dx = (int)s.tsCalXR - (int)s.tsCalXL;
  const int dy = (int)s.tsCalYB - (int)s.tsCalYT;
  if(dx > 8){
    long nx = (long)TS_CAL_LO + (long)((int)x - (int)s.tsCalXL) * (TS_CAL_XHI - TS_CAL_LO) / dx;
    if(nx < 0) nx = 0; if(nx > _width - 1) nx = _width - 1;
    x = (uint16_t)nx;
  }
  if(dy > 8){
    long ny = (long)TS_CAL_LO + (long)((int)y - (int)s.tsCalYT) * (TS_CAL_YHI - TS_CAL_LO) / dy;
    if(ny < 0) ny = 0; if(ny > _height - 1) ny = _height - 1;
    y = (uint16_t)ny;
  }
}

void TouchScreen::_point(uint16_t& x, uint16_t& y){
  if(_inject){ x = _injX; y = _injY; return; }
#if TS_MODEL==TS_MODEL_XPT2046
  TS_Point p = ts.getPoint();
  x = map(p.x, TS_X_MIN, TS_X_MAX, 0, _width);
  y = map(p.y, TS_Y_MIN, TS_Y_MAX, 0, _height);
#else
  x = ts.points[0].x;
  y = ts.points[0].y;
#endif
  /*  Під час калібрування працюємо з «сирими» координатами — тоді навіть
      зіпсована поправка не заважає перекалібруватись.  */
  if(!m2::M.calibActive()) _applyCalib(x, y);
}

volatile bool yoTouchLog = false;   /* команда touchlog: друкувати координати кожного дотику */

void TouchScreen::loop(){
  static bool wastouched = true;
  if(millis() - _touchdelay <= 10) return;       /* 100 разів на секунду: швидкий тик інакше губиться */
  _touchdelay = millis();
#if TS_I2C_PANEL
  ts.read();
#endif
  const bool istouched = _istouched();

  /*  Калібрування сенсора забирає дотики собі: по одній позначці за раз,
      координати некалібровані (див. _point).  */
  if(m2::M.calibActive()){
    uint16_t cx = _x, cy = _y;
    if(istouched) _point(cx, cy);
    if(istouched && !wastouched){ sfx.play(SFX_CLICK); m2::M.calibDown(cx, cy); }
    wastouched = istouched;
    return;
  }
#if TS_I2C_PANEL
  if(yoTouchLog && istouched && !wastouched && !_inject){
    uint16_t lx = ts.points[0].x, ly = ts.points[0].y;
    Serial.printf("##TOUCH#\tсирий rx=%u ry=%u -> екран x=%u y=%u\n", ts.rawX, ts.rawY, lx, ly);
  }
#endif

  /*  Дотик до погаслого екрана (ніч, кінець таймера сну, заставка) лише будить
      його і далі, до відпускання, нічого не натискає: людина не бачить, куди
      влучила. Кожен інший дотик продовжує денну яскравість уночі.  */
  static bool wakeOnly = false;
  if(istouched && !wastouched){
    if(extras.touchWake()) wakeOnly = true;
    if(config.isScreensaver){ wakeOnly = true; display.putRequest(NEWMODE, PLAYER); }
  }
  if(istouched && !wastouched && !wakeOnly) sfx.play(SFX_CLICK);     /* клацання — лише коли ввімкнули */
  if(wakeOnly){
    if(!istouched) wakeOnly = false;
    wastouched = istouched;
    return;
  }

  uint16_t x = _x, y = _y;
  if(istouched){
    _point(x, y);
    /*  пікселі панелі → логічні пікселі інтерфейсу (ES3C35P: ×3/4, див. m2theme.h)  */
    x = (uint16_t)m2::uiL(x); y = (uint16_t)m2::uiL(y);
  }

  /*  Відкрите меню забирає дотики собі цілком.  */
  if(yomenu.active()){
    if(istouched){
      if(!wastouched){ _x = x; _y = y; _t0 = millis(); yomenu.onPress(x, y); }
      /*  Запам'ятовуємо КОЖНУ точку ведення: при відпусканні меню отримує
          останню, а не ту, де палець почав. Через це вертикальний повзунок
          (смуга еквалайзера) стрибав назад на початок жесту — тягнеш, а
          відпускаєш, і значення повертається.  */
      else { _x = x; _y = y; yomenu.onDrag(x, y); }
    }else if(wastouched){
      yomenu.onRelease(_x, _y, millis() - _t0);
    }
    wastouched = istouched;
    return;
  }

  /*  Мережі немає чи зв'язок зник: на екрані «немає зв'язку», і дотик веде
      просто до списку мереж — там людина й вибирає свою.  */
  const bool lostLink = (display.mode() == LOST);
  const bool netReady = (network.status == CONNECTED || network.status == SDREADY) && !lostLink;
  if(!netReady){
    if(!istouched && wastouched) yomenu.openWifi(!lostLink);
    wastouched = istouched;
    return;
  }

  /*  Головний екран розбирає дотики сам.  */
  if(display.mode() == PLAYER && m2::P.shown()){
    if(istouched){
      _x = x; _y = y;
      if(!wastouched) m2::P.onPress(x, y);
      else m2::P.onDrag(x, y);
    }else if(wastouched){
      m2::P.onRelease(_x, _y);
    }
    wastouched = istouched;
    return;
  }

  /*  Будь-який інший стан (картка пам'яті читається, оновлення) — дотик нічого
      не робить, лише повертає на головний екран, коли відпустять.  */
  if(!istouched && wastouched && display.mode() != UPDATING && display.mode() != SDCHANGE)
    display.putRequest(NEWMODE, PLAYER);
  wastouched = istouched;
}

void TouchScreen::injectBegin(uint16_t x, uint16_t y){ _inject=true; _injTouched=true;  _injX=x; _injY=y; }
void TouchScreen::injectMove (uint16_t x, uint16_t y){ _inject=true; _injTouched=true;  _injX=x; _injY=y; }
/*  Знімаємо і сам прапорець підміни: інакше після відлагоджувального жесту
    справжня панель лишалася б відключеною до перезавантаження.  */
void TouchScreen::injectEnd  (){ _injTouched=false; _inject=false; }

void TouchScreen::dbgState(){
  Serial.printf("дотик: останній %u,%u режим=%d меню=%d плеєр=%d\n", _x, _y, (int)display.mode(),
                yomenu.active() ? 1 : 0, m2::P.shown() ? 1 : 0);
}

bool TouchScreen::_istouched(){
  if(_inject) return _injTouched;
#if TS_MODEL==TS_MODEL_XPT2046
  return ts.touched();
#else
  return ts.isTouched;
#endif
}

#endif  // TS_MODEL!=TS_MODEL_UNDEFINED
