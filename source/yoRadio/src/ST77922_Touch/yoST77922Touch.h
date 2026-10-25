/*  ---------------------------------------------------------------------------
 *  Сенсор, вбудований у ST77922 (плата ES3C35P): I2C 0x55, адреси регістрів
 *  16-бітні. Протокол — з драйвера esp_lcd_touch_st77922 (приклади LCDwiki):
 *    0x0000 версія, 0x000C ревізія (4 байти), 0x0005..0x0009 maxX, maxY, точок;
 *    0x0010 стан (біт 3 — є координати), 0x0009 скільки звітів,
 *    0x0014 звіти по 7 байтів: [7]=дійсний, [5:0] x старші, x молодший,
 *    y старший, y молодший, площа, сила, резерв.
 *  Координати — у власній системі панелі (320×480, портрет); read() одразу
 *  перераховує їх у екранні тим самим поворотом, що й картинка.
 *  Перевірено на платі 25.10.2026 (tools/es3c35p/probe): чотири кути тестової
 *  картини збіглися з поворотом 1 (екранний x = y панелі, y = 319 − x панелі).
 *  ------------------------------------------------------------------------- */
#ifndef yoST77922Touch_h
#define yoST77922Touch_h

#include <Arduino.h>

class ST_Point {
  public:
    ST_Point() : id(0), x(0), y(0), size(0) {}
    uint8_t  id;
    uint16_t x;
    uint16_t y;
    uint8_t  size;
};

class YoST77922Touch {
  public:
    YoST77922Touch(int8_t sda, int8_t scl, int8_t intPin, int8_t rstPin);
    bool begin(uint8_t addr = 0x55);
    uint16_t rawX = 0, rawY = 0;      /* останній сирий дотик (система панелі) */
    void setRotation(uint8_t r);      /* 1 або 3 — як у картинки (displayST77922.cpp) */
    void setResolution(uint16_t w, uint16_t h);
    void read();
    bool present() const { return _present; }
    uint8_t  fwVer = 0;
    uint8_t  fwRev[4] = { 0 };
    uint32_t reads = 0, errors = 0;               /* службове: опитів, помилок I2C */
    uint32_t touchesN = 0, resets = 0;            /* скільки дотиків почалось; скільки разів сенсор перезапускали */
    uint32_t reports = 0;                          /* скільки нових звітів сенсор віддав (біт 3 стану) */
    uint16_t releaseMs = 60;                       /* без нових звітів довше за це — палець підняли (консоль «tsrel») */
    uint16_t ghostMs = 150;                        /* фільтр «привида» після відпускання */
    uint32_t ghosts = 0;                           /* скільки «привидів» відкинуто */
    void dbgRaw();                                /* консоль «tsraw»: сирі регістри й лічильники */
    void hwReset();                               /* перезапуск сенсора виводом RST */
    void logStart(uint16_t secs);                 /* консоль «tslog»: записати кожен опит (у PSRAM) */
    void logDump();

    bool     isTouched = false;
    uint8_t  touches   = 0;
    ST_Point points[2];

  private:
    bool     _rd(uint16_t reg, uint8_t* buf, uint8_t len);
    void     _map(uint16_t rx, uint16_t ry, ST_Point& p);
    int8_t   _sda, _scl, _int, _rst;
    uint8_t  _addr = 0x55;
    uint8_t  _rotation = 1;
    uint16_t _w = 480, _h = 320;
    bool     _present = false;
    /*  Біт 3 стану — «є НОВИЙ звіт», а не «палець на склі»: сенсор шле звіти
        рідше, ніж ми питаємо (100 разів на секунду), і між ними біт порожній.
        Тому палець вважається на склі, доки звіти приходять; підняли — коли
        звітів немає довше за releaseMs. Так поводиться й FT6336 на ES3C28P,
        і на цьому стоять прокрутка, накат і повзунки.  */
    uint32_t _lastRep = 0;
    /*  «Привид»: після відпускання сенсор інколи ще раз віддає ту саму точку. Такий звіт
        у межах ghostMs після відпускання майже в тому ж місці — не новий дотик.
        25.10.2026 без цього фільтра кінець прокрутки ставав «натисканням» рядка
        під пальцем (власник: «касание для скроллинга воспринимается как нажатие»).  */
    uint32_t _relT = 0;
    uint16_t _relX = 0xFFFF, _relY = 0xFFFF;
    uint32_t _sameT = 0;                          /* відколи та сама точка без змін (сторож «завислого» дотику) */
    uint16_t _lastX = 0xFFFF, _lastY = 0xFFFF;
    bool     _was = false;
    /*  журнал опитів для налагодження (tslog)  */
    struct LogRec { uint16_t t; uint8_t st, n; uint16_t x, y; };
    LogRec*  _log = nullptr;
    uint16_t _logN = 0, _logCap = 0;
    uint32_t _logT0 = 0, _logEnd = 0;
    void     _logPoll(uint32_t now, uint8_t st, uint8_t n);
};

#endif
