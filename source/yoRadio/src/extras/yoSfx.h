/*  ---------------------------------------------------------------------------
 *  Звуки подій: увімкнення, дотик, жест, мережа, таймер сну, будильник.
 *
 *  Файли — у розділі ресурсів «assets» (LittleFS, вільні 7,9 МБ флеш-пам'яті):
 *    /snd/default/<подія>.wav — стандартні (tools/make_sounds.py),
 *    /snd/user/<подія>.wav    — свої, завантажені зі сторінки чи програми.
 *  Свій файл важливіший; видалили його — знову стандартний.
 *
 *  Коли грає станція, звук домішується до неї в Audio::playSample (після
 *  еквалайзера й гучності — у звуку своя гучність), а музика на цей час
 *  стишується. Коли радіо мовчить, звук виводить своя задача прямо в I2S,
 *  спершу розбудивши підсилювач.
 *  ------------------------------------------------------------------------- */
#ifndef yoSfx_h
#define yoSfx_h

#include <Arduino.h>

enum SfxEvent : uint8_t { SFX_START, SFX_CLICK, SFX_GESTURE, SFX_CONNECT, SFX_ERROR, SFX_TIMER, SFX_ALARM, SFX_LOWBAT, SFX_N };

class YoSfx {
  public:
    static const uint16_t DEFAULT_MASK;   /* типово озвучені події (без дотику) */

    static bool mount();                  /* розділ ресурсів — якомога раніше: з нього ж і заставка */
    static bool mounted();
    void begin();                         /* після player.init(): розділ ресурсів і задача виводу */
    void play(SfxEvent e);                /* з будь-якої задачі, не чекає; вимкнене — мовчить */
    void test(SfxEvent e);                /* перевірка зі сторінки: навіть коли подію вимкнено */
    void reload(SfxEvent e);              /* файл змінився — перечитати перед наступним разом */
    bool ready() const { return _q != nullptr; }
    bool willPlay(SfxEvent e) const;      /* подію ввімкнено й звуки є */
    uint32_t audibleMs(SfxEvent e) const { return e < SFX_N ? _audible[e] : 0; }   /* коли звук справді пішов у динамік */

    bool     fsOk() const { return _fsOk; }
    size_t   fsTotal();
    size_t   fsUsed();
    bool     userFile(SfxEvent e);
    uint16_t userMask() const { return _userMask; }   /* біт — є свій файл (запам'ятовано, без звертань до флеш) */
    void     refreshUser();
    uint32_t clipMs(SfxEvent e);          /* тривалість (завантажує, якщо ще ні) */
    static float gainDb(SfxEvent e){ const int32_t g = _gainQ15(e); return g > 0 ? 20.0f * log10f(g / 32767.0f) : -99.0f; }   /* для консолі */
    static const uint8_t MAX_SEC = 10;    /* найдовший свій звук, с */

    /*  Задача звуку, на кожен відлік: s32 — L у старших 16 бітах, R у молодших.  */
    inline bool mixing() const { return _mixPcm != nullptr; }
    uint32_t mix(uint32_t s32, uint32_t rate);

    /*  Мікрофон не має брати власні звуки радіо за хлопки.  */
    bool recent() const { return _outBusy || _mixPcm || millis() - _lastEndMs < 400; }

    static const char* id(SfxEvent e);    /* ім'я файлу без .wav */
    static const char* title(SfxEvent e); /* українською, для сторінки */
    static int         find(const char* id);

  private:
    struct Clip { int16_t* pcm; uint32_t len; uint32_t rate; bool tried; };
    Clip _clip[SFX_N] = {};
    bool _fsOk = false;
    uint16_t _userMask = 0;
    QueueHandle_t _q = nullptr;
    SemaphoreHandle_t _lock = nullptr;

    /*  стан змішування — пише задача звуків, читає задача звуку  */
    const int16_t* volatile _mixPcm = nullptr;
    volatile uint32_t _mixLen = 0;
    uint32_t _mixRate = 22050;
    uint32_t _mixStep = 0, _stepFor = 0;   /* крок у Q16 і для якої частоти він порахований */
    uint64_t _mixPos = 0;                  /* позиція в Q16 */
    int32_t  _mixGain = 0;                 /* Q15 */
    volatile bool _outBusy = false;
    volatile uint32_t _audible[SFX_N] = {};
    volatile uint32_t _lastEndMs = 0;

    static void _taskFn(void* p);
    void _run();
    const Clip* _get(SfxEvent e);
    bool _load(SfxEvent e);
    bool _synthLowBat(Clip& c);          /* типовий звук «батарея сідає», якщо файлу немає */
    void _out(const Clip& c, int32_t gainQ15, uint64_t startPos = 0, SfxEvent ev = SFX_N);
    static int32_t _gainQ15(SfxEvent e);
};

extern YoSfx sfx;

#endif
