/*  ---------------------------------------------------------------------------
 *  Оновлення з GitHub.
 *
 *  Радіо саме дивиться останній випуск у своєму репозиторії на GitHub (PR_REPO у myoptions.h:
 *  ES3C35P — roman885-85/potuzhne-radio-es3c35p, ES3C28P — roman885-85/potuzhne-radio)
 *  (через хвилину після появи мережі й далі двічі на добу) і, якщо там новіша
 *  версія, показує це в меню. Встановлення — лише за дотиком людини:
 *    1. звук зупиняється;
 *    2. качаються файли сторінки радіо (app.js.gz, app.css.gz) — у пам'ять;
 *    3. качається прошивка PotuzhneRadio-ES3C28P-update.bin і пишеться просто
 *       в другий розділ програми (Update) — стара лишається, доки нова не
 *       перевірена;
 *    4. вийшло — файли сторінки лягають у SPIFFS, радіо перезавантажується.
 *  Увесь хід (що качається, скільки вже, швидкість) видно на екрані радіо й
 *  на сторінці. Налаштування, станції й мережі не зачіпаються.
 *  ------------------------------------------------------------------------- */
#ifndef yoOta_h
#define yoOta_h
#include <Arduino.h>

enum OtaState : uint8_t { OTA_IDLE = 0, OTA_CHECKING, OTA_LATEST, OTA_AVAILABLE, OTA_ERROR,
                          OTA_PREPARE, OTA_WEB, OTA_FIRMWARE, OTA_VERIFY, OTA_DONE };

class YoOta {
  public:
    void loop();                          /* головний цикл: автоперевірка */
    void check(bool beta = false);        /* перевірити зараз (beta — з попередніми випусками) */
    void install();                       /* встановити знайдене */
    OtaState state() const { return _st; }
    bool busy() const { return _st == OTA_CHECKING || installing(); }
    bool installing() const { return _st >= OTA_PREPARE; }
    bool available() const { return _avail; }            /* знайдено новішу (навіть якщо зараз щось інше) */
    bool hasFile() const { return _urlFw[0] != 0; }      /* в останньому випуску є файл саме для цієї плати */
    uint8_t progress() const { return _pct; }             /* 0..100 поточного кроку */
    uint32_t done() const { return _got; }
    uint32_t total() const { return _size; }
    uint32_t speed() const { return _bps; }               /* байт/с */
    const char* latest() const { return _tag; }
    const char* notes() const { return _notes; }
    const char* error() const { return _err; }
    const char* stepName() const;
    uint32_t checkedAt() const { return _checkedMs; }
    static int cmpVersion(const char* a, const char* b);  /* <0 a старіша, 0 рівні, >0 новіша */
  private:
    volatile OtaState _st = OTA_IDLE;
    volatile bool _avail = false, _beta = false;
    volatile uint8_t _pct = 0;
    volatile uint32_t _got = 0, _size = 0, _bps = 0;
    uint32_t _checkedMs = 0, _nextCheck = 60000;
    char _tag[32] = { 0 };
    char _notes[640] = { 0 };
    char _err[96] = { 0 };
    char _urlFw[240] = { 0 };
    char _urlJs[240] = { 0 };
    char _urlCss[240] = { 0 };
    uint32_t _sizeFw = 0;
    static void _checkTask(void* arg);
    static void _installTask(void* arg);
    bool _doCheck();
    bool _doInstall();
    void _fail(const char* msg);
};

extern YoOta ota;

#endif
