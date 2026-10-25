/*  ---------------------------------------------------------------------------
 *  Проповіді з сайту церкви як окреме джерело.
 *
 *  /api/sermons віддає весь архів (понад 550 проповідей, 200+ КБ JSON).
 *  Читаємо потоком, по одному об'єкту, і тримаємо в PSRAM лише потрібні
 *  поля — близько 250 КБ на весь архів (читання Писання пропускаємо).
 *  Раніше брали перші 30, і власник бачив лише малу частину архіву.
 *
 *  Запит виконує окрема задача на ядрі 0: рукопожаття TLS триває під
 *  секунду, і в головному циклі, де декодер, його було б чутно.
 *  ------------------------------------------------------------------------- */
#ifndef yoSermons_h
#define yoSermons_h

#include <Arduino.h>

#define SERMON_SITE  "https://site.roman-home.keenetic.pro"
#define SERMON_MAX   900              /* місце в PSRAM; на сайті зараз 555 */
#define COVER_W      80
#define COVER_H      45
#define COVERB_W     176         /* велика — для нового головного екрана */
#define COVERB_H     99

struct Sermon {
  char     title[176];               /* найдовша назва на сайті — 170 байт */
  char     preacher[64];
  char     url[96];                  /* відносний, як у відповіді сайту */
  char     date[11];                 /* 2026-09-05 */
  char     cover[96];                /* обкладинка, відносна адреса */
  uint16_t dur;                      /* секунд */
};

class YoSermons {
  public:
    void     fetch();                /* почати завантаження, якщо ще не йде */
    bool     loading() const { return _loading; }
    uint16_t loadedSoFar() const { return _got; }   /* скільки вже прочитано, поки вантажиться */
    uint16_t count() const { return _loading ? 0 : _n; }
    uint32_t version() const { return _ver; }   /* змінюється з кожним новим списком */
    const char* error() const { return _err; }
    const Sermon* at(int i) const { return (i >= 0 && i < (int)_n && _items) ? &_items[i] : nullptr; }
    bool     play(uint16_t i);
    bool     playRel(int8_t d);        /* сусідня проповідь: кнопки ⏮ ⏭ пульта */
    int16_t  playing() const { return _playing; }
    /*  Обкладинка проповіді, що грає: 80x45 RGB565, готова для екрана.  */
    const uint16_t* coverPix() const { return _coverIdx >= 0 && _coverIdx == _playing ? _coverPix : nullptr; }
    uint32_t coverVersion() const { return _coverVer; }
    const uint16_t* coverBig() const { return _coverIdx >= 0 && _coverIdx == _playing && _coverBigOk ? _coverBig : nullptr; }
  private:
    Sermon*  _items = nullptr;
    volatile uint16_t _n = 0;
    volatile uint16_t _got = 0;
    volatile bool     _loading = false;
    volatile uint32_t _ver = 0;
    const char* _err = "";
    int16_t  _playing = -1;
    static void _task(void* arg);
    uint16_t* _coverPix = nullptr;
    uint16_t* _coverBig = nullptr;
    volatile bool _coverBigOk = false;
    volatile int16_t  _coverIdx = -1, _coverWant = -1;
    volatile bool     _coverBusy = false, _playAfterCover = false;
    volatile uint32_t _coverVer = 0;
    void _coverFetch(int16_t idx);
    static void _coverTask(void* arg);
};

extern YoSermons sermons;
void utf8FixTail(char* s);   /* прибрати недописану літеру UTF-8 в кінці */
/*  JPEG -> W x H RGB565. crop > 0 — спершу вирізати з середини прямокутник з
    пропорцією crop (ширина/висота): логотип посередині банера og:image 1200×630.  */
bool yoJpegFit(const uint8_t* jpg, size_t len, uint16_t* out, int W, int H, float crop = 0);

#endif
