/*  ---------------------------------------------------------------------------
 *  Нове меню (src/m2): каркас.
 *
 *  Сторінки лежать стеком: «Меню» → «Параметри» → «Мікрофон» → … Нова
 *  сторінка в'їжджає справа, попередня трохи відходить ліворуч і темніє;
 *  «назад» — навпаки. Вміст сторінки довший за екран прокручується пальцем
 *  з інерцією й пружиною на краях.
 *
 *  Потоки. Дотик приходить із головного циклу, малює задача дисплея. Щоб
 *  не малювати посеред зміни стану, розподіл такий:
 *   - геометрія дотику (що під пальцем, прокрутка, хвиля, повзунок під
 *     пальцем) — у задачі дисплея: події дотику лише кладуться в чергу;
 *   - дії (перемкнути, відкрити сторінку, зберегти) — у головному циклі
 *     (Menu::loop()), як і в старому меню: там можна чекати радіомодуль і
 *     писати налаштування, не зупиняючи екран;
 *   - зміна стеку сторінок, яку просить дія, кладеться в чергу й
 *     застосовується задачею дисплея на початку кадру.
 *  ------------------------------------------------------------------------- */
#ifndef m2ui_h
#define m2ui_h
#include "m2gfx.h"
#include "m2theme.h"
#include "m2icons.h"

/*  кадрів виведено (нове меню, головний екран, оновлення): задача дисплея
    бачить, що йде рух, і не спить свої 10 мс між обертами  */
extern volatile uint32_t g_m2Frames;

namespace m2 {

class Page {
  public:
    virtual ~Page(){}
    virtual const char* title() = 0;
    /*  головний цикл: перед показом / після зникнення / щооберта, поки сторінка вгорі  */
    virtual void enter(){}
    virtual void leave(){}
    virtual void loop(uint32_t now){ (void)now; }
    /*  задача дисплея: щокадру; живі значення → inval()  */
    virtual void tick(uint32_t now){ (void)now; }
    virtual int16_t height(){ return CH; }           /* висота вмісту (прокрутка, якщо > CH) */
    virtual void draw(Gfx& g) = 0;                   /* координати вмісту; тло вже намальоване */
    /*  що під пальцем: id і прямокутник (для хвилі); -1 — нічого  */
    virtual int16_t hit(int16_t x, int16_t y, Rect& r, uint8_t& radius){ (void)x; (void)y; (void)r; (void)radius; return -1; }
    /*  0 — звичайний дотик; 1 — тягнеться вбік (повзунок); 2 — одразу бере будь-який рух (смуга еквалайзера)  */
    virtual uint8_t grab(int16_t id){ (void)id; return 0; }
    /*  рух пальця по захопленому елементу (задача дисплея): лише показ і значення в чергу  */
    virtual void drag(int16_t id, int16_t x, int16_t y, bool end){ (void)id; (void)x; (void)y; (void)end; }
    /*  головний цикл  */
    virtual void tap(int16_t id, int16_t x, int16_t y){ (void)id; (void)x; (void)y; }
    virtual bool hold(int16_t id){ (void)id; return false; }        /* true — довгий дотик використано */
    virtual void value(int16_t id, int32_t v){ (void)id; (void)v; } /* значення з повзунка */
    virtual bool canBack(){ return true; }
    virtual void back();                              /* типово — на сторінку вище */
    virtual bool keepOpen(){ return false; }          /* не закривати меню за хвилину без дотиків */
    virtual bool scrollable(){ return true; }
    /*  прокрутка зупиняється рівно на рядку, з пружиною (як у старому списку): крок у пікселях, 0 — де зупинилась  */
    virtual int16_t snapStep(){ return 0; }
    /*  Службове (консоль «items»): перелік елементів для автоматичної перевірки.  */
    virtual void dump(){}
    int16_t scroll = 0;
    /*  Та сама прокрутка в пікселях панелі — для малювання. На ES3C35P логічний
        піксель = 4/3 панельного, і ціла логічна прокрутка рухала б список
        нерівними кроками (1, 1, 2 пікселі). Ставить її Menu разом зі scroll;
        якщо scroll змінили напряму, малювання бере uiP(scroll).  */
    int16_t scrollP = 0;
};

/*  ---------- типові рядки ---------- */
enum ItemType : uint8_t { IT_GAP, IT_SECTION, IT_NAV, IT_SWITCH, IT_SLIDER, IT_SEG, IT_BUTTON, IT_INFO, IT_NOTE, IT_CUSTOM };

typedef const char* (*TextFn)();
typedef int32_t (*GetFn)();
typedef void (*SetFn)(int32_t);
typedef void (*ActFn)();
typedef bool (*BoolFn)();

struct Item {
  uint8_t  type = IT_GAP;
  const char* label = nullptr;
  uint8_t  icon = 0;
  uint16_t badge = 0;
  TextFn   text = nullptr;          /* значення праворуч (NAV, INFO) / текст (NOTE) / мітка кнопки */
  GetFn    get = nullptr;
  SetFn    set = nullptr;
  ActFn    act = nullptr;
  const char* const* opts = nullptr;
  uint8_t  nopts = 0;
  int16_t  lo = 0, hi = 100;
  const char* unit = nullptr;
  BoolFn   show = nullptr;          /* показувати рядок */
  BoolFn   enabled = nullptr;       /* активний */
  uint16_t color = 0;               /* кнопка: колір тексту (0 — жовтий) */
  int16_t  h = 0;                   /* IT_CUSTOM / IT_GAP / IT_NOTE: висота */
  void   (*cdraw)(Gfx& g, int16_t x, int16_t y, int16_t w, int16_t h) = nullptr;
  /*  робоче  */
  int16_t  y = 0, hh = 0;
  bool     vis = true, first = false, last = false;
  uint32_t sig = 0;
  float    anim = 0;
  bool     ai = false;              /* anim уже має значення */
};

Item iSection(const char* label);
Item iNav(const char* label, uint8_t icon, uint16_t badge, TextFn value, ActFn act);
Item iSwitch(const char* label, uint8_t icon, uint16_t badge, GetFn get, SetFn set);
Item iSlider(const char* label, int16_t lo, int16_t hi, GetFn get, SetFn set, const char* unit = nullptr);
Item iSliderPlay(const char* label, int16_t lo, int16_t hi, GetFn get, SetFn set, const char* unit, ActFn play);   /* з кнопкою ▶ «прослухати» */
Item iSeg(const char* label, const char* const* opts, uint8_t n, GetFn get, SetFn set);
Item iButton(const char* label, uint8_t icon, ActFn act, uint16_t color = 0);
Item iInfo(const char* label, TextFn value);
Item iNote(TextFn text, int16_t h = 22);
Item iGap(int16_t h = 8);
Item iCustom(int16_t h, void (*draw)(Gfx&, int16_t, int16_t, int16_t, int16_t));

class ListPage : public Page {
  public:
    ListPage(const char* title, Item* items, uint8_t n) : _title(title), _it(items), _n(n) {}
    const char* title() override { return _title; }
    void enter() override;
    void tick(uint32_t now) override;
    int16_t height() override { return _h; }
    void draw(Gfx& g) override;
    void dump() override;
    int16_t hit(int16_t x, int16_t y, Rect& r, uint8_t& radius) override;
    uint8_t grab(int16_t id) override;
    void drag(int16_t id, int16_t x, int16_t y, bool end) override;
    void tap(int16_t id, int16_t x, int16_t y) override;
    void value(int16_t id, int32_t v) override;
  protected:
    const char* _title;
    Item* _it;
    uint8_t _n;
    int16_t _h = CH;
    uint32_t _sigT = 0;
    int16_t  _dragId = -1;            /* повзунок під пальцем: показуємо його значення, доки цикл не збереже */
    float    _dragV = 0;
    uint32_t _dragHold = 0;
    bool     _onPlay = false;          /* останній hit() влучив у кнопку ▶ повзунка */
    void _layout();
    void _drawItem(Gfx& g, Item& it);
    uint32_t _sigOf(Item& it);
    int32_t _sliderAt(const Item& it, int16_t x) const;
};

/*  ---------- спільні елементи (для власних сторінок) ---------- */
void drawSwitch(Gfx& g, int16_t x, int16_t y, float pos, bool enabled = true);   /* 40×24 */
void drawSeg(Gfx& g, int16_t x, int16_t y, int16_t w, int16_t h, const char* const* opts, uint8_t n, float pos, bool enabled = true);
void drawSlider(Gfx& g, int16_t x, int16_t y, int16_t w, float frac, bool enabled = true, float zero = -1);
void drawBadge(Gfx& g, int16_t x, int16_t y, uint8_t icon, uint16_t color);
void drawCard(Gfx& g, int16_t x, int16_t y, int16_t w, int16_t h, bool first, bool last, uint16_t c = C_SURF);

/*  ---------- меню ---------- */
class Menu {
  public:
    bool active() const { return _open || _openReq; }
    bool fading() const { return _fade != 0; }
    void open(Page* root);                /* головний цикл */
    void push(Page* p);                   /* головний цикл (з дії) */
    void pop();
    void popTo(Page* p);                  /* зняти все вище за p */
    void replace(Page* p);                /* замінити верхню без анімації «назад» */
    void close();
    void closeNow();                      /* без очікування хвилі */
    Page* top() const { return _depth ? _stack[_depth - 1] : nullptr; }
    Page* ltop() const;                   /* верхня сторінка з погляду головного циклу (черга ще не застосована) */
    uint8_t depth() const { return _depth; }

    void render();                        /* задача дисплея */
    void loop();                          /* головний цикл */

    /*  Калібрування сенсора: показує позначки по кутах, збирає дотики
        «сирими» координатами (не залежить від поточної поправки — зіпсувати
        так, щоб не можна було перекалібрувати, неможливо), перевіряє й
        зберігає. calibDown — з циклу дотику; calibActive/render — задача дисплея.  */
    void calibStart();
    void calibReset();
    bool calibActive() const;
    void calibDown(int16_t x, int16_t y);

    /*  Автоматична перевірка: перелік елементів верхньої сторінки й пряма прокрутка.  */
    void dumpTop();
    void setScroll(int16_t s);
    /*  Сила згладження прокрутки, 0..100: це ширина запасу, на який список
        має право відставати від пальця. 0 — як було, рівно за пальцем.  */
    uint8_t smooth = 100;
    void traceStart();                    /* відладка: записати хід прокрутки по тактах */
    void traceDump();
    void onPress(uint16_t x, uint16_t y);
    void onDrag(uint16_t x, uint16_t y);
    void onRelease(uint16_t x, uint16_t y);

    /*  перемалювати: прямокутник вмісту верхньої сторінки / екрана / усе  */
    void inval(const Rect& r);
    void invalScreen(int16_t x, int16_t y, int16_t w, int16_t h);
    void invalAll();
    void toast(const char* msg);
    /*  значення з повзунка — у головний цикл (не частіше, ніж раз на 80 мс; кінцеве — завжди)  */
    void postValue(int16_t id, int32_t v, bool final);
    uint32_t lastTouch() const { return _lastTouch; }

    /*  заміри кадру (команда m2perf): кадри, смуги, мікросекунди малювання й передачі  */
    uint32_t pfFrames = 0, pfStrips = 0, pfDrawUs = 0, pfXferUs = 0, pfMaxUs = 0, pfTickUs = 0;

  private:
    static const uint8_t MAXD = 10;
    Page* _stack[MAXD] = { nullptr };
    uint8_t _depth = 0;
    bool _open = false, _openReq = false;

    /*  черга змін стеку (з головного циклу в задачу дисплея)  */
    struct Cmd { uint8_t op; Page* p; };
    static const uint8_t QN = 8;
    Cmd _cmd[QN]; uint8_t _cmdH = 0, _cmdT = 0;
    /*  черга подій дотику (у задачу дисплея)  */
    struct Touch { uint8_t kind; int16_t x, y; uint32_t t; };
    Touch _tq[24]; uint8_t _tqH = 0, _tqT = 0;
    /*  черга дій (у головний цикл)  */
    struct Act { uint8_t kind; Page* p; int16_t id, x, y; int32_t v; };
    Act _aq[16]; uint8_t _aqH = 0, _aqT = 0;
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

    /*  перехід між сторінками  */
    Page* _from = nullptr;
    int8_t _tDir = 0;                     /* 1 уперед, -1 назад */
    uint32_t _tT0 = 0;
    float _tPos = 1;                      /* 0..1 */

    /*  дотик  */
    enum : uint8_t { TM_NONE, TM_UNDECIDED, TM_SCROLL, TM_GRAB, TM_BACK, TM_DEAD };
    uint8_t _tm = TM_NONE;
    int16_t _pressId = -1;
    int16_t _px0 = 0, _py0 = 0, _plx = 0, _ply = 0;
    uint32_t _pressT = 0, _lastTouch = 0;
    uint32_t _scrollEndT = 0;             /* коли відпустили палець після прокрутки */
    int16_t  _scrollEndX = 0, _scrollEndY = 0;
    bool _held = false;
    int16_t _scroll0 = 0;
    float _vel = 0, _scrollF = 0;
    bool _fling = false;
    bool _spring = false;                 /* доводка до рядка: пружина зі швидкістю наката */
    float _spX = 0, _spV = 0, _spTo = 0;
    uint32_t _spLast = 0;
    uint32_t _lastMoveT = 0;
    /*  Згладження прокрутки під пальцем. Раніше список ішов рівно за
        пальцем, один в один: палець смикнувся — смикнулась і картинка.
        Тепер список веде згладжена швидкість пальця, а до самого пальця
        його лише м'яко підтягує. Рівний рух від цього не відстає, а ривки
        усереднюються.  */
    float _dTarget = 0;                   /* куди показує палець (з гумовими краями) */
    float _dVel = 0;                      /* згладжена швидкість пальця, пікселів за секунду */
    uint32_t _dT = 0;                     /* коли востаннє рухали список */
    /*  відладка: хід прокрутки по тактах — щоб бачити згладження в числах  */
    static const uint8_t TR_N = 120;
    uint16_t _trT[TR_N]; int16_t _trS[TR_N], _trG[TR_N];
    uint8_t _trN = 0; bool _trOn = false; uint32_t _trT0 = 0;
    /*  хвиля  */
    Rect _rip; uint8_t _ripR = 0; int16_t _ripX = 0, _ripY = 0; uint32_t _ripT0 = 0; bool _ripOn = false, _ripUp = false, _ripHdr = false; uint32_t _ripUpT = 0;
    Page* _ripPage = nullptr;
    /*  повідомлення знизу  */
    char _toast[64] = { 0 }; uint32_t _toastT = 0; bool _toastOn = false;
    /*  затемнення при відкритті/закритті  */
    int8_t _fade = 0;                      /* 1 гасимо перед відкриттям, 2 засвічуємо, 3 гасимо перед закриттям */
    int8_t _fadeStep = 0;
    uint32_t _fadeTick = 0;
    uint32_t _closeAt = 0;
    uint32_t _valT = 0;
    uint32_t _frameT = 0;                 /* час поточного кадру — однаковий для всіх смуг */
    /*  що перемалювати: квадрати 16×16  */
    uint32_t _dirty[TROWS] = { 0 };
    uint8_t  _hdrMin = 255;

    void _applyCmds();
    void _processTouch();
    void _startTransition(Page* from, int8_t dir);
    void _flush();
    void _drawScene(Gfx& g);
    void _drawPage(Gfx& g, Page* p, float dx, bool overlays);   /* dx — логічний, дробовий (перехід) */
    bool _setScroll(Page* p, float f);  /* прокрутка з дробової: логічна + у пікселях панелі; true — змінилась */
    void _drawHeader(Gfx& g, Page* p);
    void _drawCalib(Gfx& g);
    void _markRaw(int16_t x, int16_t y, int16_t w, int16_t h);
    void _post(uint8_t kind, Page* p, int16_t id, int16_t x, int16_t y, int32_t v);
    void _cmdPush(uint8_t op, Page* p);
    int16_t _maxScroll(Page* p);
    void _startSnap(Page* p, uint32_t now);
    void _fadeRun();
    void _finishClose();
};

extern Menu M;

}  // namespace m2
#endif
