/*  Цей файл — гаряча точка звуку: на кожен кадр десятки множень.  */
#pragma GCC optimize ("O2")
#include "yoDsp.h"
#include "../m2/m2lang.h"
#include <math.h>
#include "esp_timer.h"
#include "yoExtras.h"
#include "yoMic.h"
#include "../core/options.h"
#include "../core/config.h"
#include "../core/player.h"

YoDsp yoDsp;

const uint16_t YoDsp::BAND_HZ[EQ_BANDS] = { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

const char* const YoDsp::PRESET_NAME[EQ_PRESETS] = { "свій", "рівно", "голос", "музика", "бас", "ніч", "яскраво", "тепло" };

/*  Пресети — дБ по смугах 31 … 16 кГц.  */
const int8_t YoDsp::PRESET[EQ_PRESETS][EQ_BANDS] = {
  {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 },   /* свій — не застосовується */
  {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 },   /* рівно */
  { -6, -5, -3, -1,  0,  2,  4,  3,  1, -1 },   /* голос: розбірливість проповіді */
  {  3,  3,  2,  0, -1, -1,  0,  2,  3,  3 },   /* музика */
  {  6,  6,  5,  3,  1,  0,  0,  0,  0,  0 },   /* бас */
  { -4, -3, -2,  0,  1,  2,  2,  0, -2, -4 },   /* ніч: м'яко, без гуркоту й шипіння */
  {  0,  0,  0, -1, -1,  0,  2,  4,  5,  5 },   /* яскраво */
  {  2,  2,  3,  2,  1,  0, -1, -2, -3, -4 },   /* тепло */
};

static const float EQ_Q = 1.2f;          /* октавні смуги, що плавно зливаються */

/*  ---------- біквадратні фільтри, «Audio EQ Cookbook» ---------- */

void YoDsp::_peak(Bq& q, float fs, float f, float Q, float db){
  float A = pow(10.0f, db / 40.0f), w = 2.0f * M_PI * f / fs, c = cos(w), al = sin(w) / (2.0f * Q);
  float a0 = 1.0f + al / A;
  q.b0 = (1.0f + al * A) / a0; q.b1 = -2.0f * c / a0; q.b2 = (1.0f - al * A) / a0;
  q.a1 = -2.0f * c / a0;       q.a2 = (1.0f - al / A) / a0;
}

void YoDsp::_lowShelf(Bq& q, float fs, float f, float db){
  float A = pow(10.0f, db / 40.0f), w = 2.0f * M_PI * f / fs, c = cos(w), s = sin(w);
  float al = s / 2.0f * sqrt(2.0f), sA = 2.0f * sqrt(A) * al;         /* нахил S = 1 */
  float a0 = (A + 1) + (A - 1) * c + sA;
  q.b0 = A * ((A + 1) - (A - 1) * c + sA) / a0;
  q.b1 = 2 * A * ((A - 1) - (A + 1) * c) / a0;
  q.b2 = A * ((A + 1) - (A - 1) * c - sA) / a0;
  q.a1 = -2 * ((A - 1) + (A + 1) * c) / a0;
  q.a2 = ((A + 1) + (A - 1) * c - sA) / a0;
}

void YoDsp::_highShelf(Bq& q, float fs, float f, float db){
  float A = pow(10.0f, db / 40.0f), w = 2.0f * M_PI * f / fs, c = cos(w), s = sin(w);
  float al = s / 2.0f * sqrt(2.0f), sA = 2.0f * sqrt(A) * al;
  float a0 = (A + 1) - (A - 1) * c + sA;
  q.b0 = A * ((A + 1) + (A - 1) * c + sA) / a0;
  q.b1 = -2 * A * ((A - 1) + (A + 1) * c) / a0;
  q.b2 = A * ((A + 1) + (A - 1) * c - sA) / a0;
  q.a1 = 2 * ((A - 1) - (A + 1) * c) / a0;
  q.a2 = ((A + 1) - (A - 1) * c - sA) / a0;
}

void YoDsp::_hpf(Bq& q, float fs, float f, float Q){
  float w = 2.0f * M_PI * f / fs, c = cos(w), al = sin(w) / (2.0f * Q), a0 = 1.0f + al;
  q.b0 = (1.0f + c) / 2.0f / a0; q.b1 = -(1.0f + c) / a0; q.b2 = q.b0;
  q.a1 = -2.0f * c / a0;         q.a2 = (1.0f - al) / a0;
}

void YoDsp::_lpf(Bq& q, float fs, float f, float Q){
  float w = 2.0f * M_PI * f / fs, c = cos(w), al = sin(w) / (2.0f * Q), a0 = 1.0f + al;
  q.b0 = (1.0f - c) / 2.0f / a0; q.b1 = (1.0f - c) / a0; q.b2 = q.b0;
  q.a1 = -2.0f * c / a0;         q.a2 = (1.0f - al) / a0;
}

float YoDsp::_magDb(const Bq& q, float fs, float hz){
  double w = 2.0 * M_PI * hz / fs, c1 = cos(w), s1 = sin(w), c2 = cos(2 * w), s2 = sin(2 * w);
  double nr = q.b0 + q.b1 * c1 + q.b2 * c2, ni = -(q.b1 * s1 + q.b2 * s2);
  double dr = 1.0 + q.a1 * c1 + q.a2 * c2,  di = -(q.a1 * s1 + q.a2 * s2);
  double num = nr * nr + ni * ni, den = dr * dr + di * di;
  if(den < 1e-30) den = 1e-30;
  if(num < 1e-30) num = 1e-30;
  return (float)(10.0 * log10(num / den));
}

/*  ---------- ланцюг за налаштуваннями ---------- */

/*  Захист і віртуальний бас: частота зрізу  */
static float guardHz(uint8_t g){ return g >= 2 ? 160.0f : 100.0f; }

uint8_t YoDsp::_build(Bq* q, float fs, uint8_t vol, uint8_t* nGuard) const {
  const ExtStore& e = extras.s;
  uint8_t n = 0;
  /*  динамік — вбудований (ES8311) або підсилювач MAX98357A; ЦАП на лінію (PCM5102A, UDA1334A)
      іде в навушники чи підсилювач, а ті бас відтворюють самі  */
  bool speaker = e.dac == 0 || e.dac == 3;
  /*  1. захист динаміка: Баттерворт 2-го порядку (м'який) або 4-го (сильний)  */
  if(speaker && e.eqGuard){
    float f = guardHz(e.eqGuard);
    if(e.eqGuard >= 2){ _hpf(q[n++], fs, f, 0.5412f); _hpf(q[n++], fs, f, 1.3066f); }
    else                _hpf(q[n++], fs, f, 0.7071f);
  }
  if(nGuard) *nGuard = n;
  /*  2. еквалайзер і поправка під кімнату — у тих самих смугах  */
  for(uint8_t b = 0; b < EQ_BANDS; b++){
    float db = 0;
    if(e.eqOn) db += e.eq[b];
    if(e.eqRoomOn) db += e.eqRoom[b];
    if(db > 18) db = 18;
    if(db < -18) db = -18;
    if(fabs(db) < 0.25f || BAND_HZ[b] > fs * 0.45f) continue;   /* рівна смуга — без ланки */
    _peak(q[n++], fs, BAND_HZ[b], EQ_Q, db);
  }
  /*  3. тонкомпенсація: чим тихіше, тим більше низу й верху (ISO 226 наближено)  */
  /*  Ланки стоять завжди (на повній гучності — з нулем дБ): коли вони
      з'являлись і зникали від гучності, мінявся склад ланок, стан фільтрів
      обнулявся — і повзунок гучності клацав.  */
  if(e.eqLoud){
    float att = vol > 0 ? -20.0f * log10(vol / 254.0f) : 48.0f;   /* на скільки дБ тихіше за повну */
    if(att < 0) att = 0;
    bool strong = e.eqLoud >= 2;
    float lo = att * (strong ? 0.40f : 0.25f), hi = att * (strong ? 0.15f : 0.10f);
    if(lo > (strong ? 12.0f : 8.0f)) lo = strong ? 12.0f : 8.0f;
    if(hi > (strong ? 5.0f : 3.0f))  hi = strong ? 5.0f : 3.0f;
    _lowShelf(q[n++], fs, speaker ? 180.0f : 100.0f, lo);
    if(8000.0f < fs * 0.45f) _highShelf(q[n++], fs, 8000.0f, hi);
  }
  return n;
}

/*  Попереднє ослаблення: пів шкали запасу було завжди (yoRadio ділив
    сигнал навпіл); більший підйом еквалайзера забираємо ще й звідси, щоб
    обмежувачу на повній гучності не доводилось притискати постійно.
    Тонкомпенсацію не рахуємо: вона велика лише на тихій гучності, де запасу
    й так досить. Рахуємо тут, а не в задачі звуку: це сотні синусів.  */
void YoDsp::changed(){
  Bq q[MAXQ];
  const float fs = 44100.0f;
  uint8_t ng = 0;
  uint8_t n = _build(q, fs, 254, &ng);          /* повна гучність — без тонкомпенсації */
  /*  Запас тримаємо не за найбільшим підйомом узагалі, а за тим, наскільки
      він здатен підняти пік справжнього сигналу. Музика вище кілогерца
      слабшає приблизно на 2 дБ на октаву, тож підйом на 16 кГц піка майже не
      чіпає. Раніше цього не враховували: повзунок 16к на +12 дБ приглушував
      УВЕСЬ звук на 6 дБ, а самого підйому на слух не було — звук просто
      ставав тихішим. Те саме, слабше, було й на інших верхніх смугах.  */
  auto head = [](float f) -> float {
    if(f <= 1000.0f) return 0.0f;
    float d = 2.0f * log2f(f / 1000.0f);
    return d > 12.0f ? 12.0f : d;
  };
  float mx = 0;
  for(float f = 25.0f; f < 20000.0f; f *= 1.26f){
    float r = -head(f);
    for(uint8_t i = 0; i < n; i++) r += _magDb(q[i], fs, f);
    if(r > mx) mx = r;
  }
  for(uint8_t b = 0; b < EQ_BANDS; b++){
    float r = -head((float)BAND_HZ[b]);
    for(uint8_t i = 0; i < n; i++) r += _magDb(q[i], fs, BAND_HZ[b]);
    if(r > mx) mx = r;
  }
  _mxDb = mx;
  _dirty = true;
}

/*  Попереднє ослаблення під підйоми — лише те, чого не дає гучність: гучність множиться
    після еквалайзера, і на гучності нижче повної запас уже є. Раніше запас брали завжди:
    +12 дБ на 31 Гц стишували весь звук на 6 дБ навіть на тихій гучності, а самого низу
    колонки майже не відтворюють — власник 26.10.2026: «на низах еквалайзер замість
    частоти регулює гучність». Тепер на повній гучності — як було, нижче — без ослаблення.
    Кличуть _recalc() і setVolume() — обидва з задачі звуку.  */
void YoDsp::_preUpdate(){
  const float att = _vol > 0 ? -20.0f * log10f((float)_vol / 254.0f) : 48.0f;
  float extra = _mxDb - 6.0f - att;
  if(extra < 0) extra = 0;
  _preDb = -6.0f - extra;
  _pre = 0.5f * powf(10.0f, -extra / 20.0f);
}

void YoDsp::_recalc(){
  const ExtStore& e = extras.s;
  float fs = (float)_fs;
  uint8_t ng = 0;
  Bq q[MAXQ];
  uint8_t n = _build(q, fs, _vol, &ng);
  /*  віртуальний бас  */
  float vbk = 0;
  if(e.vbass && e.vbass <= 3){
    float f = (e.dac == 0 && e.eqGuard) ? guardHz(e.eqGuard) : 120.0f;
    _lpf(_vbLo[0], fs, f, 0.5412f); _lpf(_vbLo[1], fs, f, 1.3066f);
    _hpf(_vbHi[0], fs, f * 1.2f, 0.7071f); _lpf(_vbHi[1], fs, f * 4.0f, 0.7071f);
    static const float K[4] = { 0, 0.9f, 1.6f, 2.6f };
    vbk = K[e.vbass];
  }
  bool fresh = _reset || n != _nq || ng != _nGuard;   /* склад ланок змінився — старий стан не пасує */
  if(_reset){
    memset(_vbLoS, 0, sizeof(_vbLoS)); memset(_vbHiS, 0, sizeof(_vbHiS));
    _lim = 1.0f;
    _reset = false;
  }
  for(uint8_t i = 0; i < n; i++){
    Stage& t = _st[i];
    t.b0 = q[i].b0; t.b1 = q[i].b1; t.b2 = q[i].b2; t.a1 = q[i].a1; t.a2 = q[i].a2;
    if(fresh){ t.l1 = t.l2 = t.r1 = t.r2 = 0; }
  }
  _nq = n; _nGuard = ng;
  _preUpdate();
  _vbK = vbk;
  _limRel = 1.0f / (0.12f * fs);                 /* відпускання ~120 мс */
  _gSm = 1.0f / (0.02f * fs);                    /* гучність — за ~20 мс */
  _volCalc = _vol;
}

__attribute__((always_inline)) static inline float runBq(const float b0, const float b1, const float b2, const float a1, const float a2, float& z1, float& z2, float x){
  float y = b0 * x + z1;
  z1 = b1 * x - a1 * y + z2;
  z2 = b2 * x - a2 * y;
  return y;
}

void YoDsp::setVolume(uint8_t vol, int8_t balance){
  /*  як Audio::Gain(): баланс забирає до vol/254·16·|bal| з одного каналу  */
  float step = (float)vol / 254.0f;
  uint8_t l = 0, r = 0;
  if(balance < 0) l = (uint8_t)(step * (float)(-balance * 16));
  if(balance > 0) r = (uint8_t)(step * (float)(balance * 16));
  _gL = (float)(vol - l) / 256.0f;
  _gR = (float)(vol - r) / 256.0f;
  const bool v = vol != _vol;
  _vol = vol; _bal = balance;
  if(v) _preUpdate();
}

uint32_t YoDsp::process(int16_t s[2]){
  bool timing = (++_n & 511) == 0;
  int64_t t0 = timing ? esp_timer_get_time() : 0;
  if(__builtin_expect(_dirty, 0) || (extras.s.eqLoud && abs((int)_vol - (int)_volCalc) >= 3)){ _dirty = false; _recalc(); }

  const float K = 1.0f / 32768.0f;
  float xl = (float)s[0] * K, xr = (float)s[1] * K;
  float vb = 0;
  if(_vbK > 0){
    float m = 0.5f * (xl + xr);
    for(uint8_t i = 0; i < 2; i++){ const Bq& q = _vbLo[i]; m = runBq(q.b0, q.b1, q.b2, q.a1, q.a2, _vbLoS[i].z1, _vbLoS[i].z2, m); }
    if(m < 0) m = -m;                             /* випрямлення: гармоніки 2f, 4f… */
    for(uint8_t i = 0; i < 2; i++){ const Bq& q = _vbHi[i]; m = runBq(q.b0, q.b1, q.b2, q.a1, q.a2, _vbHiS[i].z1, _vbHiS[i].z2, m); }
    vb = m * _vbK;
  }
  const uint8_t nq = _nq, ng = _nGuard;
  Stage* t = _st;
  for(uint8_t i = 0; i < nq; i++, t++){
    if(i == ng){ xl += vb; xr += vb; }
    float yl = t->b0 * xl + t->l1;
    t->l1 = t->b1 * xl - t->a1 * yl + t->l2;
    t->l2 = t->b2 * xl - t->a2 * yl;
    float yr = t->b0 * xr + t->r1;
    t->r1 = t->b1 * xr - t->a1 * yr + t->r2;
    t->r2 = t->b2 * xr - t->a2 * yr;
    xl = yl; xr = yr;
  }
  if(ng == nq){ xl += vb; xr += vb; }
  if(__builtin_expect(_gLc < 0, 0)){ _gLc = _gL; _gRc = _gR; _prec = _pre; }   /* перший відлік — без наїзду */
  _prec += (_pre - _prec) * _gSm;
  const float pre = _prec;
  xl *= pre; xr *= pre;

  /*  для покажчика рівня — як і було: у пів шкали, до гучності  */
  int32_t vl = (int32_t)(xl * 32768.0f), vr = (int32_t)(xr * 32768.0f);
  s[0] = vl > 32767 ? 32767 : vl < -32768 ? -32768 : vl;
  s[1] = vr > 32767 ? 32767 : vr < -32768 ? -32768 : vr;

  if(__builtin_expect(_testGain >= 0, 0)){ xl *= _testGain; xr *= _testGain; }
  else {
    const float sm = _gSm;
    _gLc += (_gL - _gLc) * sm; _gRc += (_gR - _gRc) * sm;
    xl *= _gLc; xr *= _gRc;
  }

  /*  м'який пуск / зупинка  */
  if(__builtin_expect(_fade != _fadeTo || _fade < 1.0f, 0)){
    const float step = 1000.0f / ((float)(_fadeMs ? _fadeMs : 1) * (float)_fs);
    float f = _fade;
    if(f < _fadeTo){ f += step; if(f > _fadeTo) f = _fadeTo; }
    else if(f > _fadeTo){ f -= step; if(f < _fadeTo) f = _fadeTo; }
    _fade = f;
    /*  квадратична крива — на слух рівніше за пряму  */
    const float g = f * f;
    xl *= g; xr *= g;
  }

  /*  обмежувач: миттєвий напад, відпускання ~120 мс, спільний для каналів  */
  const float T = 0.97f;
  float al = xl < 0 ? -xl : xl, ar = xr < 0 ? -xr : xr;
  float pk = al > ar ? al : ar;
  float lim = _lim;
  if(pk * lim > T){ lim = T / pk; _limHits++; }
  else if(lim < 1.0f){ lim += (1.0f - lim) * _limRel; if(lim > 1.0f) lim = 1.0f; }
  _lim = lim;
  xl *= lim; xr *= lim;

  int32_t ol = (int32_t)(xl * 32767.0f), orr = (int32_t)(xr * 32767.0f);
  if(ol > 32767) ol = 32767; else if(ol < -32768) ol = -32768;
  if(orr > 32767) orr = 32767; else if(orr < -32768) orr = -32768;
  if(timing){
    float us = (float)(esp_timer_get_time() - t0);
    _usAvg += (us - _usAvg) * 0.05f;
  }
  return ((uint32_t)(uint16_t)ol << 16) | (uint16_t)orr;
}

/*  Скільки коштує кадр: n кадрів шуму через увесь ланцюг, без очікування
    I2S. Лише коли плеєр стоїть — стан фільтрів спільний із задачею звуку.  */
float YoDsp::bench(uint32_t frames){
  int16_t s[2];
  uint32_t r = 12345;
  _dirty = true;
  int64_t t0 = esp_timer_get_time();
  for(uint32_t i = 0; i < frames; i++){
    r = r * 1664525UL + 1013904223UL;
    s[0] = (int16_t)(r >> 17); s[1] = (int16_t)(r >> 15);
    process(s);
  }
  return (float)(esp_timer_get_time() - t0) / frames;
}

/*  ---------- налаштування ---------- */

void YoDsp::applyPreset(uint8_t p){
  if(p >= EQ_PRESETS) return;
  extras.s.eqPreset = p;
  if(p) memcpy(extras.s.eq, PRESET[p], EQ_BANDS);
  extras.s.eqOn = 1;
  extras.changed();
  changed();
}

void YoDsp::setBand(uint8_t band, int8_t db){
  if(band >= EQ_BANDS) return;
  if(db > 12) db = 12;
  if(db < -12) db = -12;
  extras.s.eq[band] = db;
  extras.s.eqPreset = 0;
  extras.s.eqOn = 1;
  extras.changed();
  changed();
}

/*  Старі низькі (полиця 80 Гц), середні (пік 3 кГц) і високі (полиця 6 кГц),
    −16..+16 дБ — у найближчі смуги. Для сторінки yoRadio й перенесення
    налаштувань, що вже були.  */
void YoDsp::fromTone(int8_t bass, int8_t middle, int8_t treble){
  /*  старі фільтри підіймали не більше ніж на 6 дБ (IIR_calculateCoefficients
      обрізав −40..+6) — так і переносимо, щоб звук не змінився  */
  if(bass > 6) bass = 6;
  if(middle > 6) middle = 6;
  if(treble > 6) treble = 6;
  int t[EQ_BANDS] = { bass, bass, bass, bass / 2, 0, middle / 3, middle * 2 / 3, middle + treble / 3, treble, treble };
  for(uint8_t b = 0; b < EQ_BANDS; b++) extras.s.eq[b] = (int8_t)(t[b] > 12 ? 12 : t[b] < -12 ? -12 : t[b]);
  extras.s.eqPreset = 0;
  extras.s.eqOn = 1;
  extras.changed();
  changed();
}

void YoDsp::roomClear(){
  memset(extras.s.eqRoom, 0, EQ_BANDS);
  extras.s.eqRoomOn = 0;
  extras.changed();
  changed();
}

float YoDsp::responseDb(float hz) const {
  Bq q[MAXQ];
  float fs = (float)_fs;
  uint8_t n = _build(q, fs, _vol, nullptr);
  float r = 0;
  for(uint8_t i = 0; i < n; i++) r += _magDb(q[i], fs, hz);
  return r;
}

/*  ---------- налаштування під кімнату ----------

    Замір — терції 63 Гц…16 кГц (yoMic::sweep); у кожну октавну смугу зводимо
    три терції навколо неї (середня потужність), беремо лише ті, де тон
    вибивається з фону на 10 дБ. Нерівність — стандартне відхилення смуг
    500 Гц…16 кГц: нижче маленький динамік не звучить, і там не правимо.

    Мікрофон не калібрований і стоїть поруч із динаміком, тому одному заміру
    не віримо:
     1) спершу перевіряємо, чи тракт лінійний: тон 1 кГц на двох рівнях, на
        12 дБ різних, мусить і в мікрофоні дати 12 дБ різниці, а відліки — не
        впиратись у стелю АЦП. Ні — зменшуємо підсилення мікрофона чи
        гучність тонів і пробуємо знову;
     2) базовий замір → поправка (70 % різниці, підйом до +6 дБ);
     3) контрольний замір уже з поправкою; що лишилось — доправляємо, до
        трьох разів; зберігаємо найкращий варіант;
     4) якщо нерівність не зменшилась хоч на 1 дБ — поправку не вмикаємо.  */

static const int8_t ROOM_MAP[EQ_BANDS][3] = {
  {-1,-1,-1}, {0,1,-1}, {2,3,4}, {5,6,7}, {8,9,10}, {11,12,13}, {14,15,16}, {17,18,19}, {20,21,22}, {23,24,-1} };

static bool sweepBands(float lvl[EQ_BANDS], bool ok[EQ_BANDS]){
  int n = 0;
  for(uint8_t b = 0; b < EQ_BANDS; b++){
    double pw = 0; int cnt = 0;
    for(uint8_t k = 0; k < 3; k++){
      int8_t i = ROOM_MAP[b][k];
      if(i < 0) continue;
      float d = mic.sweepDb(i), nz = mic.sweepNoise(i);
      if(isnan(d) || d - nz < 10.0f) continue;
      pw += pow(10.0, d / 10.0); cnt++;
    }
    ok[b] = cnt > 0;
    lvl[b] = cnt ? (float)(10.0 * log10(pw / cnt)) : 0;
    if(cnt) n++;
  }
  return n >= 5;
}

/*  опора — середнє 500 Гц…4 кГц; нерівність — відхилення смуг 500 Гц…16 кГц  */
static float bandsRef(const float* lvl, const bool* ok){
  float r = 0; int c = 0;
  for(uint8_t b = 4; b <= 7; b++) if(ok[b]){ r += lvl[b]; c++; }
  return c ? r / c : 0;
}
static float bandsSpread(const float* lvl, const bool* ok){
  float m = 0; int c = 0;
  for(uint8_t b = 4; b < EQ_BANDS; b++) if(ok[b]){ m += lvl[b]; c++; }
  if(c < 3) return 99;
  m /= c;
  float v = 0;
  for(uint8_t b = 4; b < EQ_BANDS; b++) if(ok[b]) v += (lvl[b] - m) * (lvl[b] - m);
  return sqrt(v / c);
}

bool YoDsp::roomFromSweep(char* why, size_t n){
  float lvl[EQ_BANDS]; bool ok[EQ_BANDS];
  if(mic.sweepState() != 2 || !mic.sweepAmp() || !sweepBands(lvl, ok)){
    snprintf(why, n, "немає придатного заміру");
    return false;
  }
  float ref = bandsRef(lvl, ok);
  for(uint8_t b = 0; b < EQ_BANDS; b++){
    float c = 0;
    if(ok[b] && lvl[b] - ref > -15.0f){ c = -(lvl[b] - ref) * 0.7f; }
    if(c > 6) c = 6; if(c < -9) c = -9;
    extras.s.eqRoom[b] = (int8_t)lroundf(c);
  }
  extras.s.eqRoomOn = 1;
  extras.changed();
  changed();
  snprintf(why, n, "готово");
  return true;
}

uint8_t YoDsp::roomProgress() const {
  if(_rtState >= RT_DONE) return 100;
  if(_rtState == RT_IDLE) return 0;
  /*  лінійність 0–8 %, базовий 8–36 %, контрольні — по 21 %  */
  float in = mic.sweepState() == 1 ? (float)mic.sweepPos() / YoMic::SWEEP_N : 0;
  switch(_rtState){
    case RT_STOP: return 0;
    case RT_LIN:  return (uint8_t)(_rtLinStep * 4);
    case RT_BASE: return (uint8_t)(8 + in * 28);
    case RT_VERIFY: return (uint8_t)(36 + (_rtIter - 1) * 21 + in * 21);
  }
  return 0;
}

const char* YoDsp::roomTuneStart(){
  if(_rtState > RT_IDLE && _rtState < RT_DONE) return "уже налаштовую";
  if(!mic.running())  return "мікрофон не запущено";
  if(extras.s.dac)    return "звук іде на зовнішній ЦАП";
  _rtResume = player.isRunning();
  if(_rtResume) player.sendCommand({PR_STOP, 0});
  _rtState = RT_STOP; _rtT = millis();
  _rtBefore = _rtAfter = -1;
  snprintf(_rtMsg, sizeof(_rtMsg), "зупиняю звук");
  return nullptr;
}

bool YoDsp::_rtSweep(int8_t level, uint32_t mask){
  const char* why = mic.sweepStart(level, true, mask, true);
  if(why){ _rtFinish(false, why); return false; }
  return true;
}

/*  Кінець процедури: повернути людині її налаштування, підсилення мікрофона,
    звук — і сказати, чим закінчилось.  */
void YoDsp::_rtFinish(bool ok, const char* msg){
  ExtStore& e = extras.s;
  _testGain = -1.0f;
  e.eqOn = _rtSave[0]; e.eqLoud = _rtSave[2]; e.vbass = _rtSave[3];
  if(!ok){
    memcpy(e.eqRoom, _rtOld, EQ_BANDS);
    e.eqRoomOn = _rtSave[1];
  }
  mic.apply();
  extras.changed();
  changed();
  snprintf(_rtMsg, sizeof(_rtMsg), "%s", m2::tr(msg));
  _rtState = ok ? RT_DONE : RT_FAIL;
  Serial.printf("##ROOM#\t%s (нерівність %.1f -> %.1f дБ)\n", msg, _rtBefore, _rtAfter);
  if(_rtResume) player.sendCommand({PR_PLAY, config.lastStation()});
  _rtResume = false;
}

void YoDsp::roomTick(){
  if(_rtState == RT_IDLE || _rtState >= RT_DONE) return;
  ExtStore& e = extras.s;
  if(_rtState == RT_STOP){
    if(player.isRunning()){
      if(millis() - _rtT > 4000){ _rtState = RT_FAIL; snprintf(_rtMsg, sizeof(_rtMsg), "звук не зупиняється"); _rtResume = false; }
      return;
    }
    _rtSave[0] = e.eqOn; _rtSave[1] = e.eqRoomOn; _rtSave[2] = e.eqLoud; _rtSave[3] = e.vbass;
    memcpy(_rtOld, e.eqRoom, EQ_BANDS);
    e.eqOn = 0; e.eqRoomOn = 0; e.eqLoud = 0; e.vbass = 0;       /* рівно; захист динаміка лишається */
    changed();
    _rtGain = e.micGain ? (e.micGain > 1 ? e.micGain - 1 : 0) : 4;
    _testGain = 0.35f;
    _rtLinStep = 0; _rtLinTries = 0;
    _rtState = RT_LIN;
    snprintf(_rtMsg, sizeof(_rtMsg), "перевіряю мікрофон");
    _rtSweep(-12, 1UL << 12);                                    /* 1 кГц, гучніше */
    return;
  }
  if(mic.sweepState() == 1) return;                              /* замір ще йде */
  if(mic.sweepState() != 2){ _rtFinish(false, "перервано"); return; }

  if(_rtState == RT_LIN){
    if(_rtLinStep == 0){
      _rtLinA = mic.sweepDb(12); _rtLinPeak = mic.sweepPeak();
      _rtLinStep = 1;
      _rtSweep(-24, 1UL << 12);                                  /* той самий тон на 12 дБ тихіше */
      return;
    }
    float diff = _rtLinA - mic.sweepDb(12);
    bool clip = _rtLinPeak > 29000;
    bool lin = !isnan(diff) && fabs(diff - 12.0f) <= 2.0f && !clip;
    Serial.printf("##ROOM#\tлінійність: різниця %.1f дБ (має бути 12), пік %d, підсилення %u, гучність тонів %.2f\n",
                  diff, (int)_rtLinPeak, (unsigned)_rtGain, _testGain);
    if(!lin){
      if(++_rtLinTries > 4){ _rtFinish(false, "мікрофон спотворює звук"); return; }
      /*  зменшуємо спершу підсилення мікрофона, далі — гучність тонів  */
      if(_rtGain >= 2){ _rtGain -= 2; mic.gainTemp(_rtGain); }
      else if(_testGain > 0.06f) _testGain *= 0.5f;
      else { _rtFinish(false, "мікрофон спотворює звук"); return; }
      _rtLinStep = 0;
      _rtSweep(-12, 1UL << 12);
      return;
    }
    _rtState = RT_BASE;
    snprintf(_rtMsg, sizeof(_rtMsg), "слухаю тони");
    _rtSweep(-12, 0);
    return;
  }

  float lvl[EQ_BANDS]; bool ok[EQ_BANDS];
  if(!sweepBands(lvl, ok)){ _rtFinish(false, "мікрофон не чує тонів"); return; }
  float ref = bandsRef(lvl, ok), spread = bandsSpread(lvl, ok);

  if(_rtState == RT_BASE){
    _rtBefore = spread;
    for(uint8_t b = 0; b < EQ_BANDS; b++){
      _rtCan[b] = ok[b] && lvl[b] - ref > -15.0f;                /* тут динамік звучить — можна правити */
      float c = _rtCan[b] ? -(lvl[b] - ref) * 0.7f : 0;
      if(c > 6) c = 6; if(c < -9) c = -9;
      _rtCorr[b] = c;
      e.eqRoom[b] = (int8_t)lroundf(c);
    }
    memcpy(_rtBest, e.eqRoom, EQ_BANDS);
    _rtBestSpread = _rtBefore;
    e.eqRoomOn = 1;
    changed();
    _rtIter = 1;
    _rtState = RT_VERIFY;
    snprintf(_rtMsg, sizeof(_rtMsg), "перевіряю результат");
    _rtSweep(-12, 0);
    return;
  }

  if(_rtState == RT_VERIFY){
    Serial.printf("##ROOM#\tпрохід %u: нерівність %.1f дБ (було %.1f)\n", (unsigned)_rtIter, spread, _rtBefore);
    bool better = spread < _rtBestSpread - 0.2f;
    if(better){ _rtBestSpread = spread; memcpy(_rtBest, e.eqRoom, EQ_BANDS); }
    if(_rtIter < 3 && better){
      /*  що лишилось нерівним — доправляємо (60 % залишку)  */
      for(uint8_t b = 0; b < EQ_BANDS; b++){
        if(!_rtCan[b] || !ok[b]) continue;
        float c = _rtCorr[b] - (lvl[b] - ref) * 0.6f;
        if(c > 6) c = 6; if(c < -9) c = -9;
        _rtCorr[b] = c;
        e.eqRoom[b] = (int8_t)lroundf(c);
      }
      changed();
      _rtIter++;
      snprintf(_rtMsg, sizeof(_rtMsg), m2::tr("уточнюю (%u)"), (unsigned)_rtIter);
      _rtSweep(-12, 0);
      return;
    }
    memcpy(e.eqRoom, _rtBest, EQ_BANDS);
    _rtAfter = _rtBestSpread;
    char m[48];
    if(_rtBestSpread <= _rtBefore - 1.0f){
      e.eqRoomOn = 1;
      snprintf(m, sizeof(m), m2::tr("рівніше: %.1f -> %.1f дБ"), _rtBefore, _rtBestSpread);
      _rtFinish(true, m);
    }else{
      e.eqRoomOn = 0;
      snprintf(m, sizeof(m), m2::tr("поправка не потрібна (%.1f дБ)"), _rtBefore);
      _rtFinish(true, m);
    }
  }
}
