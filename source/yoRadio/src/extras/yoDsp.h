/*  ---------------------------------------------------------------------------
 *  Обробка звуку перед виходом: замість трьох фільтрів yoRadio (низькі,
 *  середні, високі) — повний ланцюг під можливості цього радіо.
 *
 *   захист динаміка → віртуальний бас → поправка під кімнату → еквалайзер
 *   на 10 смуг → тонкомпенсація → попереднє ослаблення → гучність і баланс →
 *   обмежувач
 *
 *  - Еквалайзер: 10 октавних смуг 31 Гц…16 кГц, ±12 дБ, пікові біквадратні
 *    фільтри за «Audio EQ Cookbook» (R. Bristow-Johnson).
 *  - Поправка під кімнату: радіо грає тони й слухає себе мікрофоном
 *    (yoMic::sweep), різницю з рівною характеристикою віднімає.
 *  - Захист динаміка: зріз низу, якого маленький динамік не відтворює, а
 *    лише хрипить (замір: на 63–100 Гц друга гармоніка всього на 11–19 дБ
 *    нижче тону).
 *  - Віртуальний бас: низ, що зрізано, відтворюється його гармоніками — вухо
 *    саме «добудовує» основний тон (ефект відсутньої основної частоти;
 *    нелінійний пристрій — двополуперіодне випрямлення, як у класичних
 *    схемах MaxxBass / Larsen & Aarts).
 *  - Тонкомпенсація: на тихій гучності вухо гірше чує низ і верх (ISO 226),
 *    тож їх підіймаємо тим більше, чим тихіше.
 *  - Обмежувач: сума підйомів не переповнює 16 біт — замість клацань
 *    м'яко притискаємо пік.
 *
 *  Коефіцієнти перераховує сама задача звуку, коли щось змінилось: меню й
 *  сторінка лише ставлять прапорець, тож напівзаписаних фільтрів не буває.
 *  ------------------------------------------------------------------------- */
#pragma once
#include <Arduino.h>

#define EQ_BANDS    10
#define EQ_PRESETS  8

class YoDsp {
  public:
    static const uint16_t BAND_HZ[EQ_BANDS];
    static const char* const PRESET_NAME[EQ_PRESETS];
    static const int8_t PRESET[EQ_PRESETS][EQ_BANDS];

    void changed();                              /* налаштування в extras.s змінились (не з задачі звуку) */
    void setRate(uint32_t fs) { if(fs >= 8000 && fs != _fs){ _fs = fs; _reset = true; _dirty = true; } }
    void setVolume(uint8_t vol, int8_t balance);

    /*  Один кадр: s — вхід, на виході в s те, що бачить покажчик рівня (до
        гучності, у пів шкали, як і було); повертає кадр для I2S (L<<16 | R).  */
    uint32_t process(int16_t s[2]);

    /*  М'який пуск і зупинка: множник звуку, що плавно їде до 0 чи 1 по відліках
        (поки звуку немає, він стоїть — нарощування почнеться з першим відліком).  */
    void fadeIn(uint16_t ms)  { _fade = 0.0f; _fadeTo = 1.0f; _fadeMs = ms; }
    void fadeOut(uint16_t ms) { _fadeTo = 0.0f; _fadeMs = ms; }
    bool faded() const { return _fade <= 0.001f; }
    void fadeReset() { _fade = 1.0f; _fadeTo = 1.0f; }
    float fadeLevel() const { return _fade; }

    void applyPreset(uint8_t p);                 /* 0 — «свій», нічого не міняє */
    void setBand(uint8_t band, int8_t db);       /* повзунок: пресет стає «свій» */
    bool roomFromSweep(char* why, size_t n);     /* поправка під кімнату з останнього заміру */
    void roomClear();
    void fromTone(int8_t bass, int8_t middle, int8_t treble);   /* три повзунки yoRadio → смуги */

    /*  Налаштування під кімнату — від кнопки до результату, без людини:
        зупинити звук, рівні налаштування (захист лишається), тони через увесь
        ланцюг на помірній гучності, поправка, повернути все й грати далі.
        Кличе головний цикл (roomTick).  */
    const char* roomTuneStart();                 /* nullptr — почали */
    void     roomTick();
    enum { RT_IDLE = 0, RT_STOP, RT_LIN, RT_BASE, RT_VERIFY, RT_DONE = 10, RT_FAIL = 11 };
    /*  для сторінок: 0 ні, 1 зайнято (зупиняю / міряю), 3 готово, 4 не вийшло  */
    uint8_t  roomState() const { return _rtState == RT_IDLE ? 0 : _rtState < RT_DONE ? (_rtState == RT_STOP ? 1 : 2) : _rtState == RT_DONE ? 3 : 4; }
    float    roomBefore() const { return _rtBefore; }   /* нерівність до й після, дБ (-1 — не міряли) */
    float    roomAfter() const { return _rtAfter; }
    const char* roomMsg() const { return _rtMsg; }
    uint8_t  roomProgress() const;               /* 0..100 */

    /*  АЧХ того, що налаштовано (без обмежувача й віртуального басу), дБ —
        для сторінки й самоперевірки. Рахує окремо від задачі звуку.  */
    float responseDb(float hz) const;
    float preampDb() const { return _preDb; }

    /*  навантаження: мікросекунд на кадр (середнє за останні заміри)  */
    float usPerFrame() const { return _usAvg; }
    uint32_t limitHits() const { return _limHits; }
    float    bench(uint32_t frames);            /* мкс на кадр на шумі; лише коли плеєр стоїть */
    uint8_t  stages() const { return _nq; }

  private:
    volatile float _fade = 1.0f, _fadeTo = 1.0f;
    volatile uint16_t _fadeMs = 300;
  
    struct Bq { float b0, b1, b2, a1, a2; };
    struct St { float z1, z2; };
    /*  ланка разом зі станом обох каналів — підряд у пам'яті  */
    struct Stage { float b0, b1, b2, a1, a2, l1, l2, r1, r2; };
    enum { MAXQ = 24 };
    Stage _st[MAXQ];
    uint8_t _nq = 0;
    /*  віртуальний бас: моно-гілка  */
    Bq  _vbLo[2], _vbHi[2];
    St  _vbLoS[2], _vbHiS[2];
    float _vbK = 0;
    uint8_t _nGuard = 0;          /* скільки перших ланок — захист (гілка басу бере вхід до них) */

    volatile bool _dirty = true, _reset = true;
    uint32_t _fs = 44100;
    volatile uint8_t _vol = 64;
    volatile int8_t  _bal = 0;
    volatile float   _gL = 0.25f, _gR = 0.25f;   /* гучність із балансом, пораховані заздалегідь */
    uint8_t _volCalc = 255;       /* гучність, під яку пораховано тонкомпенсацію */
    float _pre = 0.5f, _preDb = -6.0f;
    volatile float _mxDb = 0;     /* найбільший підйом АЧХ (з поправкою на спад музики вгорі) — для запасу */
    void  _preUpdate();
    float _lim = 1.0f, _limRel = 0.0003f;
    /*  гучність і попереднє ослаблення міняються не стрибком, а за ~20 мс:
        стрибок підсилення посеред хвилі клацав у динаміку, коли тягнули повзунок  */
    float _gLc = -1.0f, _gRc = 0.0f, _prec = 0.5f, _gSm = 0.0011f;
    uint32_t _limHits = 0;
    uint32_t _n = 0;
    float _usAvg = 0;
    volatile float _testGain = -1.0f;         /* замір кімнати: гучність замість людської */
    uint8_t  _rtState = 0;
    uint32_t _rtT = 0;
    bool     _rtResume = false;
    uint8_t  _rtSave[4] = {0};                /* eqOn, eqRoomOn, eqLoud, vbass */
    int8_t   _rtOld[EQ_BANDS] = {0}, _rtBest[EQ_BANDS] = {0};
    float    _rtCorr[EQ_BANDS] = {0};
    bool     _rtCan[EQ_BANDS] = {0};
    float    _rtBefore = -1, _rtAfter = -1, _rtBestSpread = 99, _rtLinA = 0;
    int16_t  _rtLinPeak = 0;
    uint8_t  _rtIter = 0, _rtLinStep = 0, _rtLinTries = 0, _rtGain = 4;
    bool     _rtSweep(int8_t level, uint32_t mask);
    void     _rtFinish(bool ok, const char* msg);
    char     _rtMsg[48] = {0};

    void _recalc();
    static void _peak(Bq& q, float fs, float f, float Q, float db);
    static void _lowShelf(Bq& q, float fs, float f, float db);
    static void _highShelf(Bq& q, float fs, float f, float db);
    static void _hpf(Bq& q, float fs, float f, float Q);
    static void _lpf(Bq& q, float fs, float f, float Q);
    static float _magDb(const Bq& q, float fs, float hz);
    uint8_t _build(Bq* q, float fs, uint8_t vol, uint8_t* nGuard) const;   /* ланки за налаштуваннями */
};

extern YoDsp yoDsp;
