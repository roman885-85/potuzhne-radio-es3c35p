/*  ---------------------------------------------------------------------------
 *  Бездротова колонка: AirPlay (перша версія, RAOP).
 *
 *  iPhone, iPad і Mac бачать радіо у списку AirPlay («Звук» у Пункті керування,
 *  кнопка AirPlay у Музиці, подкастах, YouTube) і грають на ньому будь-який
 *  свій звук. На відміну від DLNA, тут телефон не дає посилання, а сам шле
 *  звук: 44,1 кГц, 16 біт, стерео, стиснений без втрат (ALAC) і зашифрований.
 *
 *  Як це влаштовано:
 *   - mDNS «_raop._tcp» — радіо з'являється у списку пристроїв;
 *   - RTSP на порту 5000 — рукопожаття (підпис ключем AirPort Express,
 *     розшифрування ключа AES), параметри потоку, гучність, назва доріжки;
 *   - UDP 6000/6001/6002 — звук, повтори загублених пакетів, час;
 *   - пакети розшифровуються й декодуються в окремій задачі та лягають у
 *     буфер у PSRAM (до 4 с); головний цикл бере звідти відліки й подає їх
 *     у ту саму обробку, що й станції (еквалайзер, гучність, звуки подій).
 *
 *  Ключ AirPort Express (ним Apple перевіряє колонку й шифрує ключ потоку) у
 *  репозиторій не кладемо: радіо один раз бере його з відкритого приймача
 *  shairport-sync (закріплена версія файла), звіряє SHA-256 і зберігає в NVS.
 *
 *  AirPlay 2 (кілька кімнат, HomeKit) так не зробити: там потрібен чип
 *  сертифікації Apple.
 *  ------------------------------------------------------------------------- */
#pragma once
#include <Arduino.h>

class YoAirplay {
  public:
    void begin();                       /* мережа піднялась */
    void stop();                        /* вимкнули в налаштуваннях */
    bool on() const;                    /* увімкнено в налаштуваннях */
    void setOn(bool v);
    void loop();                        /* головний цикл: ключ і оголошення в мережі */
    bool ready() const { return _keyOk; }          /* ключ є — радіо видно в AirPlay */
    const char* keyState() const { return _keyErr; }
    bool session() const { return _session; }   /* хтось під'єднаний */
    const char* device() const { return _device; }  /* хто грає (ім'я телефона чи Mac) */

    /*  Для плеєра (головний цикл).  */
    size_t read(int16_t* out, size_t maxFrames);   /* стерео-відліки; 0 — поки нема */
    void released();                    /* радіо перемкнули на інше — AirPlay мовчить до наступного «грати» */
    bool takeTitle(char* out, size_t n);           /* нова назва доріжки, якщо прийшла */
    void stat();                        /* налагодження: команда «airplay» */

  private:
    friend void yoAirplayTask(void*);
    void _run();
    void _accept();
    void _read();
    void _request(char* req, size_t hdrLen, char* body, size_t bodyLen);
    void _packet(uint8_t* p, uint16_t n, uint8_t kind, int64_t tArr);
    void _store(uint16_t seq, uint32_t ts, const int16_t* pcm, uint16_t frames);
    size_t _readTimed(int16_t* out);   /* звук за часом (пакети 0x54) — під м'ютексом */
    void _resend(uint16_t first, uint16_t count);
    void _timing();
    void _reset();                      /* очистити буфер (FLUSH, новий потік) */
    void _end();                        /* сесія скінчилась */
    bool _mdns();
    bool _loadKey();
    static void _keyTask(void*);
    volatile bool _keyOk = false;
    volatile bool _keyBusy = false;
    uint32_t _keyNext = 0;
    char _keyErr[64] = {0};
    volatile bool _session = false;
    volatile bool _released = false;
    bool _outReq = false;               /* уже попросили плеєр перейти на AirPlay */
    bool _idleSent = false;
    uint32_t _lastPkt = 0;
    bool _mdnsOn = false;
    char _device[48] = {0};
    char _title[160] = {0};
    volatile bool _titleNew = false;
    void* _task = nullptr;
};

extern YoAirplay airplay;
