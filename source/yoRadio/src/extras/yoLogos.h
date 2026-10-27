/*  ---------------------------------------------------------------------------
 *  Логотипи станцій — самі, без комп'ютера.
 *
 *  Заграла станція, а логотипа для неї ще немає — шукаємо її в каталозі
 *  radio-browser.info за адресою потоку (запасний шлях — точний збіг назви),
 *  качаємо значок, розпаковуємо (PNG, ICO, JPEG), зменшуємо до 45x45 і
 *  кладемо в SPIFFS готовими пікселями: /logo/<crc32 адреси>.565.
 *  Не знайшли — ставимо позначку .no, щоб не питати щоразу; мережеві збої
 *  позначки не ставлять — спробуємо іншим разом.
 *
 *  PNG розбираємо самі: розпаковувач zlib (tinfl) уже є в ПЗУ ESP32-S3.
 *  ------------------------------------------------------------------------- */
#ifndef yoLogos_h
#define yoLogos_h

#include <Arduino.h>

#define LOGO_S   45

class YoLogos {
  public:
    bool     want(const char* url, const char* name);   /* з задачі дисплея; false — пошук не почався (зайнято, нема мережі, пам'яті) */
    uint32_t version() const { return _ver; }          /* змінюється, коли з'явився новий логотип */
    bool     busy() const { return _busy; }
    uint16_t forget();                                  /* стерти позначки «нема» — шукати знову */
    void     bump() { _ver++; }                         /* логотип змінили ззовні (веб) — перечитати */
    static uint32_t crc(const char* url);
    /*  Усі станції списку разом (25.10.2026, прохання власника: «отдельная функция для
        загрузок иконок, чтобы это выполнять единоразово»). По одній, у тлі, тим самим
        пошуком; ті, що вже є або позначені «нема», пропускає. Сама — раз, коли вперше
        з'явилась мережа (позначка /logo/all.ok); ще — кнопка в «Розробнику», команда logoall.  */
    void     fetchAll();
    void     fetchAllForce();                           /* кнопка: прохід навіть якщо вже був */
    void     wipeAll(){ _bWipe = true; fetchAllForce(); }   /* налагодження (logowipe): стерти всі й шукати наново */
    bool     isNo(uint32_t crc) const;                  /* у списку «нема логотипа» (список у пам'яті, не файл на станцію) */
    void     loop();                                    /* з головного циклу */
    bool     test(const char* favUrl);                  /* налагодження (logotest): скачати й розпакувати один значок */
    /*  Обкладинка проповіді просить мережу собі: прохід по станціях чекає (hold), а вона —
        поки скінчиться поточний запит логотипа (netBusy). Два TLS разом у пам'ять не влазять.  */
    void     hold(bool on){ _hold = on; }
    bool     netBusy() const;
    bool     batchOn() const { return _bOn; }
    uint16_t batchDone() const { return _bI; }
    uint16_t batchTotal() const { return _bN; }
    uint16_t batchFound() const { return _bFound; }
  private:
    volatile bool     _busy = false;
    volatile bool     _quick = false;                   /* пошук зі списку — без паузи на підключення станції */
    volatile bool     _bOn = false, _bPending = false;
    volatile uint16_t _bI = 0, _bN = 0;
    volatile uint16_t _bFound = 0, _bNet = 0;
    volatile bool _bForce = false;                      /* прохід навіть якщо вже був (кнопка) */
    volatile bool _bForget = false;                     /* перед проходом скинути позначки «нема» */
    volatile bool _bWipe = false;                       /* перед проходом стерти всі логотипи (logowipe) */
    volatile bool _hold = false;
    int      _one();                                    /* пошук для _url/_name (у задачі) */
    char*    _tu = nullptr;                             /* адреса значка для test() (PSRAM) */
    static void _testTask(void* arg);
    static void _allTask(void* arg);
    volatile uint32_t _ver = 0;
    char     _url[200] = {0};
    char     _name[64] = {0};
    uint32_t _crc = 0;
    static void _task(void* arg);
};

extern YoLogos logos;

/*  Розмір самого знака в готовій клітинці LOGO_S×LOGO_S і чи його прочитати.  */
void yoLogoContent(const uint16_t* px, int& bw, int& bh);
bool yoLogoReadable(int bw, int bh);

/*  Розпакувати зображення (PNG / ICO / JPEG) у W x H RGB565, прозоре — на білому
    (світлий знак на прозорому — на темній підкладці).  */
/*  crop > 0 — вирізати з середини прямокутник з пропорцією crop (банер og:image).  */
bool yoImageFit(const uint8_t* data, size_t len, uint16_t* out, int W, int H, float crop = 0);

#endif
