/*  ---------------------------------------------------------------------------
 *  Новий головний екран (плеєр) у стилі нового меню.
 *
 *  Весь екран малює ця сцена — тими самими смугами через DMA, що й нове
 *  меню: шапка зі станцією й кнопкою меню, картка «зараз грає», великий
 *  годинник з датою й погодою, рядок обраного / пульт картки / рівень звуку
 *  і гучність. Малюється лише змінене.
 *
 *  Дотики сюди приходять із головного циклу (touchscreen.cpp): дії —
 *  одразу там, а що підсвітити й перемалювати — прапорцями для задачі дисплея.
 *  ------------------------------------------------------------------------- */
#ifndef m2player_h
#define m2player_h
#include "m2ui.h"

namespace m2 {

/*  p2perf (консоль): час етапів кадру головного екрана — m2player.cpp  */
extern volatile uint32_t p2Frames, p2Strips, p2Px, p2BgUs, p2DrawUs, p2PushUs, p2EndUs, p2MaxUs;
extern volatile uint32_t p2Calls, p2PreUs, p2SpecUs, p2LoopGap;

class Player {
  public:
    void volAnimFrom(float v) { _volShownF = v; }   /* консоль volanim: показати рух повзунка, гучність не міняючи */
    bool on() const { return true; }     /* старого вигляду більше немає */
    void reloadLogo(){ _logoKey = 0xFFFFFFFF; invalAll(); }   /* «шукати логотипи знову» */
    void show();                         /* задача дисплея: плеєр щойно став на екран */
    void hide(){ _shown = false; }
    bool shown() const { return _shown; }
    void render();                       /* задача дисплея, щооберта, поки плеєр на екрані */
    void invalAll(){ _mark(0, 0, SW, SH); }
    int16_t _cardH() const;              /* висота картки: з обкладинкою проповіді — більша */
    /*  задача дисплея: стан замість старих діалогів yoRadio (жовта смуга посередині)  */
    void setStatus(uint8_t st){ _status = st; _statusN = -1; }
    void setStatusCount(int32_t n){ _statusN = n; }
    /*  головний цикл  */
    void onPress(int16_t x, int16_t y);
    void onDrag(int16_t x, int16_t y);
    void onRelease(int16_t x, int16_t y);
  private:
    volatile bool _shown = false;
    uint32_t _dirty[TROWS] = { 0 };
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
    uint16_t* _bg = nullptr;             /* тло з підсвіткою кольором логотипа (PSRAM) */
    uint16_t  _bgColor = 0xFFFF;
    uint16_t* _logo = nullptr;           /* логотип 58×58 */
    bool      _logoOk = false;
    uint32_t  _logoKey = 0xFFFFFFFF;
    uint32_t  _logoRetryT = 0;           /* пошук логотипа не почався — коли спробувати знову */
    uint16_t  _glow = 0;
    uint16_t* _favPix[6] = { nullptr };   /* логотипи обраного 26×26 */
    bool      _favOk[6] = { false };
    uint32_t  _favKey = 0xFFFFFFFF;
    /*  підписи, щоб знати, що змінилось  */
    uint32_t _sTop = 0, _sCard = 0, _sClock = 0, _sSec = 0, _sRow = 0, _sVol = 0, _sigT = 0;
    uint32_t _t0Name = 0, _t0Title = 0;
    /*  біжучі рядки: крок рівно в піксель кожні 25 мс (не за часом — інакше пізній кадр перескакує)  */
    int16_t  _offName = 0, _offTitle = 0;
    uint32_t _mqT = 0, _barT = 0;
    uint32_t _frameT = 0;                 /* час поточного кадру — однаковий для всіх смуг */
    float    _specH[32] = { 0 };          /* намальована висота рисок (дробова — край згладжений) */
    uint32_t _animT = 0, _specT = 0;
    float    _spec[32] = { 0 };
    float    _bars[5] = { 0 };
    /*  дотик  */
    volatile int8_t _zone = -1;          /* 0 шапка-джерело, 1 шапка-назва, 2 меню, 3 картка, 4 рядок, 5 гучність, 6 годинник */
    volatile int16_t _px = 0, _py = 0, _lx = 0, _ly = 0;
    volatile uint32_t _pt = 0, _upT = 0;
    /*  хвиля по картці живе своїм часом: дотягується до кінця й гасне після
        відпускання; на кадр береться знімок, щоб усі смуги малювали одне й те саме  */
    volatile bool _ripOn = false, _ripRel = false;
    volatile int16_t _ripX = 0, _ripY = 0;
    volatile uint32_t _ripT0 = 0, _ripUpT = 0;
    struct { bool on, rel; int16_t x, y; uint32_t t0, up; } _rf = { false, false, 0, 0, 0, 0 };
    volatile bool _down = false;
    volatile int16_t _volDrag = -1;       /* гучність під пальцем (0..254) */
    float _volShownF = -1;                /* показана гучність: плавно наздоганяє ціль */
    uint32_t _volAnimT = 0;               /* мить попереднього кроку анімації гучності */
    uint32_t _volSent = 0; int16_t _volLast = -1;
    volatile float _seek = -1;           /* пульт: куди перемотає */
    volatile int8_t _btn = -1;           /* пульт: натиснута кнопка */
    int16_t _rowMode = -1;               /* 0 рівень, 1 обране, 2 пульт */
    /*  пропозиція оновитись: показуємо, доки людина не вибере; «пізніше» — до наступного запуску  */
    char     _dismiss[32] = { 0 };
    volatile int8_t _popup = 0;          /* 0 немає, 1 нова версія, 2 оновлення не вдалося, 3 батарея сідає, 4 немає зв'язку, 5 картка, 6 оновлення файлом */
    volatile int8_t _popBtn = -1;        /* натиснута кнопка в картці */
    bool     _otaWasInstalling = false;
    uint32_t _lowSeen = 0, _lowUntil = 0;  /* показ попередження про батарею (6 с на кожне) */
    volatile uint8_t _status = 0;        /* стан радіо замість старих діалогів: 1 немає зв'язку, 2 картка, 3 оновлення */
    volatile int32_t _statusN = -1;      /* лічильник (файли картки) */
    static Rect _popRect(){ return Rect(16, 44, SW - 32, 152); }
    void _drawPopup(Gfx& g);

    void _mark(int16_t x, int16_t y, int16_t w, int16_t h);
    void _flush();
    void _draw(Gfx& g);
    void _drawTop(Gfx& g, uint32_t now);
    void _drawCard(Gfx& g, uint32_t now);
    void _drawClock(Gfx& g);
    void _drawRow(Gfx& g);
    void _drawVol(Gfx& g);
    void _ripple(Gfx& g);
    void _loadLogo();
    void _loadFav();
    void _makeBg(uint16_t glow);
    uint8_t _mode() const;               /* що в третьому рядку */
    int16_t _statusLeft() const;
};

extern Player P;

}  // namespace m2
#endif
