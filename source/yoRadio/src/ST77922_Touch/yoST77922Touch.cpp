#include "../core/options.h"
#if TS_MODEL==TS_MODEL_ST77922

#include "yoST77922Touch.h"
#include <Wire.h>
#include "esp_heap_caps.h"

#define ST_FW_VERSION     0x0000
#define ST_FW_REVISION    0x000C
#define ST_MAX_X_H        0x0005
#define ST_MAX_TOUCHES    0x0009
#define ST_TOUCH_INFO     0x0010
#define ST_REPORT_0       0x0014

#define ST_PANEL_W  320          /* панель у власній, портретній системі */
#define ST_PANEL_H  480

YoST77922Touch::YoST77922Touch(int8_t sda, int8_t scl, int8_t intPin, int8_t rstPin)
  : _sda(sda), _scl(scl), _int(intPin), _rst(rstPin) {}

bool YoST77922Touch::_rd(uint16_t reg, uint8_t* buf, uint8_t len){
  Wire.beginTransmission(_addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)reg);
  if(Wire.endTransmission(false) != 0){ errors++; return false; }
  if(Wire.requestFrom(_addr, len) != len){ errors++; return false; }
  for(uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

void YoST77922Touch::hwReset(){
  if(_rst < 0) return;
  digitalWrite(_rst, LOW);  delay(20);
  digitalWrite(_rst, HIGH); delay(100);
  resets++;
}

void YoST77922Touch::dbgRaw(){
  uint8_t st = 0, mx = 0, rep[35] = { 0 };
  const bool a = _rd(ST_TOUCH_INFO, &st, 1), b = _rd(ST_MAX_TOUCHES, &mx, 1);
  const uint8_t n = mx > 5 ? 5 : mx;
  const bool c = n && _rd(ST_REPORT_0, rep, n * 7);
  Serial.printf("TSRAW стан 0x%02X%s, комірок %u%s, INT=%d, дотик=%d (%u,%u) сирий (%u,%u)\n", st, a ? "" : "(помилка)", mx, b ? "" : "(помилка)",
                _int >= 0 ? digitalRead(_int) : -1, isTouched ? 1 : 0, points[0].x, points[0].y, rawX, rawY);
  if(c) for(uint8_t i = 0; i < n; i++){ const uint8_t* p = rep + i * 7; Serial.printf("  комірка %u: %02X %02X %02X %02X %02X %02X %02X\n", i, p[0], p[1], p[2], p[3], p[4], p[5], p[6]); }
  Serial.printf("TSRAW опитів %u, звітів %u, помилок I2C %u, дотиків %u, перезапусків %u, відпускання після %u мс без звітів, «привидів» відкинуто %u\n",
                (unsigned)reads, (unsigned)reports, (unsigned)errors, (unsigned)touchesN, (unsigned)resets, (unsigned)releaseMs, (unsigned)ghosts);
}

bool YoST77922Touch::begin(uint8_t addr){
  _addr = addr;
  Wire.begin(_sda, _scl, 400000);
  if(_int >= 0) pinMode(_int, INPUT_PULLUP);
  if(_rst >= 0){
    /*  як у драйвері виробника: 100 мс у нулі, 100 мс на прокидання  */
    pinMode(_rst, OUTPUT);
    digitalWrite(_rst, LOW);  delay(100);
    digitalWrite(_rst, HIGH); delay(100);
  }
  uint8_t info[5] = { 0 };
  _present = _rd(ST_FW_VERSION, &fwVer, 1) && _rd(ST_FW_REVISION, fwRev, 4) && _rd(ST_MAX_X_H, info, 5);
  if(!_present) Serial.println("##[ERROR]#\tсенсор ST77922 не відповідає по I2C (0x55)");
  else Serial.printf("##[BOOT]#\tсенсор ST77922: версія %u (%u.%u.%u.%u), поле %ux%u, точок %u\n", fwVer,
                     fwRev[0], fwRev[1], fwRev[2], fwRev[3], (info[0] << 8) | info[1], (info[2] << 8) | info[3], info[4]);
  return _present;
}

void YoST77922Touch::setRotation(uint8_t r){ _rotation = r; }
void YoST77922Touch::setResolution(uint16_t w, uint16_t h){ _w = w; _h = h; }

void YoST77922Touch::_map(uint16_t rx, uint16_t ry, ST_Point& p){
  if(rx >= ST_PANEL_W) rx = ST_PANEL_W - 1;
  if(ry >= ST_PANEL_H) ry = ST_PANEL_H - 1;
  if(_rotation == 3){ p.x = ST_PANEL_H - 1 - ry; p.y = rx; }
  else              { p.x = ry;                  p.y = ST_PANEL_W - 1 - rx; }
  if(p.x >= _w) p.x = _w - 1;
  if(p.y >= _h) p.y = _h - 1;
}

void YoST77922Touch::read(){
  if(!_present){ isTouched = false; touches = 0; return; }
  reads++;
  const uint32_t now = millis();
  uint8_t st = 0, n = 0;
  if(!_rd(ST_TOUCH_INFO, &st, 1)){ _logPoll(now, 0xFF, 0); return; }   /* збій I2C — стан не міняємо */
  if(st & 0x08){
    /*  новий звіт: дочитуємо ВСІ комірки (0x0009 — їх кількість, у цього сенсора 5),
        як драйвер виробника, — з неповним читанням сенсор після першого дотику замовкав  */
    reports++;
    uint8_t mx = 0;
    if(_rd(ST_MAX_TOUCHES, &mx, 1) && mx){
      if(mx > 10) mx = 10;
      uint8_t rep[70];
      if(_rd(ST_REPORT_0, rep, mx * 7)){
        for(uint8_t i = 0; i < mx && n < 2; i++){            /* радіо бере лише перші два пальці */
          const uint8_t* p = rep + i * 7;
          if(!(p[0] & 0x80)) continue;
          uint16_t rx = ((p[0] & 0x3F) << 8) | p[1], ry = ((p[2] & 0x3F) << 8) | p[3];   /* старші біти — прапорці (як у XiaoZhi) */
          if(n == 0){ rawX = rx; rawY = ry; }
          points[n].id = i; points[n].size = p[4];
          _map(rx, ry, points[n]);
          n++;
        }
      }
    }
    if(n && !isTouched && _relT && now - _relT < ghostMs &&
       abs((int)rawX - (int)_relX) < 24 && abs((int)rawY - (int)_relY) < 24){
      ghosts++; _relT = now;                               /* «привид» після відпускання — не дотик (серія — теж) */
    }
    else if(n){ isTouched = true; touches = n; _lastRep = now; }
    else { isTouched = false; touches = 0; }               /* звіт без жодної точки — палець підняли */
  }else if(isTouched && now - _lastRep > releaseMs){
    isTouched = false; touches = 0;                        /* звіти скінчились — палець підняли */
  }
  _logPoll(now, st, isTouched ? touches : 0);
  /*  Сторож: та сама точка, не зрушивши ні на піксель, 20 с поспіль — так палець
      не тримають; найімовірніше, сенсор «завис» на старому звіті. Перезапуск.  */
  if(isTouched && _was && rawX == _lastX && rawY == _lastY){
    if(!_sameT) _sameT = now;
    else if(now - _sameT > 20000){ Serial.println("##TOUCH#\tсенсор 20 с віддає ту саму точку — перезапуск"); hwReset(); _sameT = 0; isTouched = false; touches = 0; }
  }else _sameT = 0;
  if(isTouched && !_was) touchesN++;
  if(isTouched){ _lastX = rawX; _lastY = rawY; }
  if(!isTouched && _was){ _relT = now; _relX = _lastX; _relY = _lastY; }   /* відпустили — звідси вікно «привида» */
  _was = isTouched;
}

/*  ---------- журнал опитів (tslog) ---------- */
void YoST77922Touch::logStart(uint16_t secs){
  if(!_log){ _logCap = 4000; _log = (LogRec*)heap_caps_malloc(sizeof(LogRec) * _logCap, MALLOC_CAP_SPIRAM); }
  if(!_log){ Serial.println("TSLOG: немає пам'яті"); return; }
  _logN = 0; _logT0 = millis(); _logEnd = _logT0 + (uint32_t)secs * 1000;
  Serial.printf("TSLOG пишу %u с — торкайтесь і ведіть пальцем\n", secs);
}
void YoST77922Touch::_logPoll(uint32_t now, uint8_t st, uint8_t n){
  if(!_log || !_logEnd || _logN >= _logCap) return;
  if((int32_t)(now - _logEnd) > 0){ _logEnd = 0; Serial.printf("TSLOG записано %u опитів — tslog dump\n", _logN); return; }
  _log[_logN++] = { (uint16_t)(now - _logT0), st, n, points[0].x, points[0].y };
}
void YoST77922Touch::logDump(){
  if(!_log){ Serial.println("TSLOG порожній"); return; }
  uint16_t newRep = 0, held = 0; uint16_t maxGap = 0, lastRepT = 0; bool had = false;
  for(uint16_t i = 0; i < _logN; i++){
    const LogRec& r = _log[i];
    if(r.st != 0xFF && (r.st & 0x08)){ if(had && r.n){ uint16_t g = r.t - lastRepT; if(g > maxGap) maxGap = g; } lastRepT = r.t; had = r.n; newRep++; }
    if(r.n) held++;
  }
  Serial.printf("TSLOG опитів %u, нових звітів %u, опитів «палець на склі» %u, найбільший проміжок між звітами під пальцем %u мс\n", _logN, newRep, held, maxGap);
  for(uint16_t i = 0; i < _logN; i++){
    const LogRec& r = _log[i];
    if(!r.n && !(r.st != 0xFF && (r.st & 0x08)) && (i == 0 || !_log[i - 1].n)) continue;   /* тиша — не друкуємо */
    Serial.printf("  %5u мс  стан %02X  дотик %u  %3u,%3u\n", r.t, r.st, r.n, r.x, r.y);
  }
  Serial.println("TSLOG кінець");
}

#endif  // TS_MODEL_ST77922
