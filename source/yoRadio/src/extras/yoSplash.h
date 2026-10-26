/*  ---------------------------------------------------------------------------
 *  Анімована заставка запуску: файл /splash.yan у розділі ресурсів
 *  (tools/make_splash.py). Кадри готові — радіо лише розпаковує зміни й
 *  шле їх у дисплей через DMA, поки підключається до мережі. Після вступу
 *  крутиться петля, доки плеєр не готовий.
 *  ------------------------------------------------------------------------- */
#ifndef yoSplash_h
#define yoSplash_h
#include <Arduino.h>

class YoSplash {
  public:
    /*  з задачі дисплея; false — файлу немає. sync — почати разом зі звуком увімкнення  */
    bool begin(bool sync = true, const char* path = "/splash.yan");
    void tick();                                     /* з задачі дисплея: наступний кадр, якщо час */
    void stop();
    bool active() const { return _data != nullptr; }
    bool introDone() const { return _cur >= _loop; }
    /*  Чекати, поки вступ докаже, ще варто: від першого кадру минуло менше за вступ із запасом
        (не довше 4 с), а перший кадр — не пізніше 12 с від увімкнення.  */
    bool worthWaiting() const;
  private:
    uint8_t*  _data = nullptr;
    size_t    _size = 0;
    uint32_t* _offs = nullptr;
    uint16_t  _w = 0, _h = 0, _n = 0, _fps = 25, _loop = 0, _wrap = 0;
    uint16_t  _cur = 0;
    uint32_t  _nextMs = 0;
    bool      _hold = false;
    uint32_t  _holdSince = 0;
    uint32_t  _firstMs = 0;                           /* коли показано перший кадр (0 — ще ні) */
    uint8_t*  _px = nullptr;                          /* пікселі смуги, внутрішня пам'ять для DMA */
    uint16_t* _cv = nullptr;                          /* ES3C35P: полотно кадру (PSRAM) — смуги панелі кратні 8 рядкам */
    uint16_t  _cw = 0;                                /* ширина полотна (кратна 8) */
    void _draw(uint16_t idx);
};

extern YoSplash splash;
#endif
