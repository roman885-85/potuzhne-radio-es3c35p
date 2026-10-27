#ifndef network_h
#define network_h
#include <WiFi.h>

enum n_Status_e { CONNECTED, SOFT_AP, FAILED, SDREADY };
/*  Спроба підключитись на прохання людини: без перезавантаження, з
    негайною відповіддю — не вийшло через пароль чи мережі просто немає.  */
enum n_Try_e { TRY_NONE, TRY_RUN, TRY_OK, TRY_BADPASS, TRY_NOTFOUND, TRY_FAIL };

class MyNetwork {
  public:
    n_Status_e status;
    struct tm timeinfo;
    bool lostPlaying = false, beginReconnect = false;
    /*  Мережа зникла: повертаємось у неї самі — по одній спробі за раз,
        не блокуючи ні звук, ні екран, ні пошук мереж у меню.  */
    bool linkLost = false;
    bool staPaused = false;          /* спроби спинено: шукаємо мережі */
    /*  mDNS піднявся і ним можна користуватись. Прапорець обов'язковий:
        бібліотека mDNS не переживає звернення до себе, поки не запущена
        (assert усередині черги -> падіння системи), а падіння в setup()
        дає нескінченне перезавантаження радіо.  */
    bool mdnsReady = false;
  public:
    MyNetwork() {};
    void begin();
    void loop();                     /* з головного циклу: повернення в мережу */
    void pauseSta(bool on);          /* на час пошуку мереж звільняємо радіомодуль */
    uint8_t lostTries() const { return _reTry; }
  public:
    void requestTimeSync(bool withTelnetOutput=false, uint8_t clientId=0);
    void requestWeatherSync();
    void setWifiParams();
    bool mdnsStart();                /* підняти mDNS; можна кликати повторно */
    bool wifiBegin(bool silent=false);
    bool wifiRemembered();           /* мережа, яку пам'ятає сам драйвер Wi-Fi */
    void connectTo(const char* ssid, const char* pass);   /* спроба просто зараз */
    n_Try_e tryState() const { return _try; }
    const char* trySsid() const { return _tryS; }
    void tryClear();                 /* спробу закрито; не вийшло — повертаємось до збережених мереж */
    /*  Меню саме показує підсумок спроби — не закривати її за нього. Інакше
        спробу, з якої вийшли не кнопкою, закриває сам loop() за 20 с.  */
    volatile bool tryHeld = false;
    void dump();                     /* відладка: стан повернення в мережу */
    /*  Запасна адреса: з точкою з'єдналися, а роутер не дає адреси (DHCP мовчить) — радіо бере
        свою минулу адресу в цій мережі (якщо її ніхто не зайняв). Див. _dhcpFallback.  */
    bool dhcpSilent() const { return _assocAt && !_fbOn && millis() - _assocAt > 10000; }
    bool dhcpFallbackOn() const { return _fbOn; }
    static void skipBootWifi();      /* відладка: наступний старт — наче жодної мережі поруч */
  private:
    uint32_t _reAt = 0;              /* час наступної спроби */
    uint32_t _pauseAt = 0;           /* коли спинили спроби */
    uint8_t  _reTry = 0;             /* скільки спроб поспіль не вдалося */
    uint8_t  _reNext = 0;            /* яку зі збережених мереж пробувати далі */
    int8_t   _reCur = -1;            /* яку пробуємо зараз — щоб запам'ятати вдалу */
    volatile n_Try_e _try = TRY_NONE;
    char     _tryS[33] = {0}, _tryP[65] = {0};
    uint32_t _tryAt = 0;
    uint32_t _tryEndAt = 0;          /* коли спроба скінчилась (успіх чи відмова) */
    bool     _bootNoNet = false;     /* стартували без мережі: служби й станції ще не підняті */
    bool     _staReady = false;      /* setWifiParams уже зроблено */
    bool     _apWas = false;         /* на час спроби точку доступу прибрали */
    uint8_t  _tryAgain = 0;          /* друга спроба: перший відмов буває хибним */
    bool     _tryEv = false;         /* на цю спробу вже прийшла відповідь драйвера */
    bool     _evReady = false;       /* події Wi-Fi уже підписані */
    void     _staUp();               /* мережа з'явилась на ходу: підняти служби */
    void _noNetwork();               /* мережі немає: чекаємо, доки людина вибере */
    static void WiFiLostConnection(WiFiEvent_t event, WiFiEventInfo_t info);
    static void WiFiAssoc(WiFiEvent_t event, WiFiEventInfo_t info);
    volatile uint32_t _assocAt = 0; /* коли з'єднались із точкою (адреси ще нема); 0 — ні */
    bool     _fbOn = false;          /* працюємо на своїй минулій адресі: DHCP мовчав */
    bool     _fbTried = false;       /* на цьому з'єднанні запасну вже пробували */
    void     _dhcpFallback();        /* DHCP мовчить 10 с — узяти минулу адресу */
    void     _dhcpBack();            /* перед новою спробою — знову DHCP */
    static void WiFiReconnected(WiFiEvent_t event, WiFiEventInfo_t info);
};

extern MyNetwork network;

extern __attribute__((weak)) void network_on_connect();

#endif
