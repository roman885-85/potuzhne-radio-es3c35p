#ifndef player_h
#define player_h

#if I2S_DOUT!=255 || I2S_INTERNAL
  #include "../audioI2S/AudioEx.h"
#else
  #include "../audioVS1053/audioVS1053Ex.h"
#endif

#ifndef MQTT_BURL_SIZE
  #define MQTT_BURL_SIZE  512
#endif
/*  Відтворення довільного посилання (browseUrl) потрібне проповідям із
    сайту, тож вмикаємо його й без MQTT.  */
#define YO_BROWSEURL

#ifndef PLQ_SEND_DELAY
  /*  Черга плеєра на п'ять місць. Якщо її ніхто не розгрібає (а без мережі
      player.loop() не викликався зовсім), то кожна посилка чекала цілу
      секунду — і це заморожувало того, хто посилає. А посилає задача екрана,
      двічі на дві секунди: екран оживав раз на дві секунди, дотик наче
      зависав. Команда рівня чи перевірки картки не варта жодної секунди —
      краще її загубити.  */
  #define PLQ_SEND_DELAY pdMS_TO_TICKS(20) //portMAX_DELAY
#endif

enum playerRequestType_e : uint8_t { PR_PLAY = 1, PR_STOP = 2, PR_PREV = 3, PR_NEXT = 4, PR_VOL = 5, PR_CHECKSD = 6, PR_VUTONUS = 7, PR_BURL = 8, PR_TOGGLE = 9, PR_EXT = 10, PR_SEEK = 11 };   /* PR_EXT: 1 — грати звук AirPlay, 0 — AirPlay скінчився; PR_SEEK — позиція у файлі картки, байти */
struct playerRequestParams_t
{
  playerRequestType_e type;
  int payload;
};

enum plStatus_e : uint8_t{ PLAYING = 1, STOPPED = 2 };

class Player: public Audio {
  private:
    uint32_t    _volTicks;   /* delayed volume save  */
    bool        _volTimer;   /* delayed volume save  */
    uint32_t    _resumeFilePos;
    plStatus_e  _status;
    //char        _plError[PLERR_LN];
  private:
    volatile uint32_t _seekTo = 0, _seekAt = 0;   /* куди просили перемотати і коли */
    volatile uint32_t _cPos = 0, _cSize = 0, _cTime = 0, _cDur = 0, _cFill = 0, _cAt = 0;
    void _stop(bool alreadyStopped = false, bool keepAmp = false);
    void _fadeOutWait(uint16_t ms = 300);   /* звук грає — спершу плавно стишити */
    void _play(uint16_t stationId);
    void _loadVol(uint8_t volume);
    bool _hasError;
    void _extStart();                   /* перейти на звук AirPlay */
    void _extRelease(bool byRadio);     /* зійти з AirPlay: byRadio — радіо перемкнули на інше */
    void _extPump();                    /* відліки AirPlay → обробка звуку → I2S */
    bool _extData = false;
    /*  Потік станції обірвався, і бібліотека не змогла з'єднатись знову — пробуємо самі ще кілька
        разів (власник 03.11.2026: радіо стало саме посеред вечора, «Host not available»).  */
    uint8_t  _retryN = 0;               /* скільки повторів уже було підряд */
    uint32_t _retryAt = 0;              /* коли наступний (0 — не чекаємо) */
    uint16_t _retrySt = 0;              /* яку станцію */
    bool     _autoPlay = false;         /* наступний PR_PLAY — сам (повтор, автостарт), а не людина */
    uint32_t _playT0 = 0;               /* коли станція з'єдналась — після 30 с гри лічильник повторів скидається */
    void _retrySchedule();
  public:
    void autoPlay(uint16_t st);         /* грати станцію самому: автостарт; не вийде — повтори */
    bool lockOutput = true;
    volatile bool extOn = false;     /* грає AirPlay (extras/yoAirplay): звук іде не з потоку, а готовими відліками */
    bool resumeAfterUrl = false;
    volatile bool connproc = true;
    uint32_t sd_min, sd_max;
    #if defined(MQTT_ROOT_TOPIC) || defined(YO_BROWSEURL)
    char      burl[MQTT_BURL_SIZE];  /* buffer for browseUrl  */
    char      burlTitle[96] = {0};   /* другий рядок під назвою: хто проповідує */
    /*  Перемотка проповіді: сервер віддає файл з будь-якого байта (Range),
        тож секунду переводимо в байт через розмір і тривалість.  */
    uint32_t  burlSize = 0;          /* повний розмір файлу, з першого з'єднання */
    uint32_t  burlDur = 0;           /* тривалість, с — із сайту */
    uint32_t  burlBase = 0;          /* з якої секунди йде поточне з'єднання */
    uint32_t  burlResume = 0;        /* де зупинились — звідти й продовжимо */
    bool      burlRanged = false;
    int8_t    burlResumeRadio = -1;  /* 1/0 — чи грало радіо до проповіді (задає той, хто зупинив потік) */
    void      burlSeek(uint32_t sec);
    #endif
  public:
    Player();
    void init();
    void loop();
    void initHeaders(const char *file);
    void setError();
    void setError(const char *e);
    //bool hasError() { return strlen(_plError)>0; }
    void sendCommand(playerRequestParams_t request);
    /*  Перемотка файла картки: запам'ятовуємо, куди попросили, і доки команда
        не виконалась — саме це й показуємо програмам. Інакше повзунок у них
        стрибає: вони вже пересунули його, а радіо ще кілька разів встигає
        відповісти старою позицією.  */
    void     seekTo(uint32_t pos);
    /*  Те, що віддаємо сторінці й програмам. Читати тут напряму з audiofile
        НЕ можна: getFilePos()/getFileSize() кличуть audiofile.position() і
        .size(), тобто лізуть у файловий об'єкт SD із задачі веб-сервера, поки
        головний цикл читає з того самого файла. Значення знімаємо в
        головному циклі (cachePos) і віддаємо копію.  */
    void     cachePos();
    uint32_t shownFilePos();
    uint32_t shownFileSize(){ return _cSize; }
    uint32_t shownTime(){ return _cTime; }
    uint32_t shownDur(){ return _cDur; }
    void resetQueue();
    #if defined(MQTT_ROOT_TOPIC) || defined(YO_BROWSEURL)
    void browseUrl();
    #endif
    bool remoteStationName = false;
    plStatus_e status() { return _status; }
    void prev();
    void next();
    void toggle();
    void fadeStop();                 /* одразу, але через затихання (перемикання джерела) — з головного циклу */
    void stepVol(bool up);
    void setVol(uint8_t volume);
    uint8_t volToI2S(uint8_t volume);
    void stopInfo();
    void setOutputPins(bool isPlaying);
    void setResumeFilePos(uint32_t pos) { _resumeFilePos = pos; }
    /*  Будильник і таймер сну ведуть гучність самі: поки тут не -1, запуск
        станції бере це значення, а не збережене, і не вмикається на повну
        посеред наростання. Налаштування при цьому не змінюються.  */
    int16_t volOverride = -1;
    void applyVol(uint8_t v) { setVolume(volToI2S(v)); }
    uint32_t posSec();               /* де грає: для проповіді — з урахуванням перемотки */
    uint32_t durSec();
};

extern Player player;

extern __attribute__((weak)) void player_on_start_play();
extern __attribute__((weak)) void player_on_stop_play();
extern __attribute__((weak)) void player_on_track_change();
extern __attribute__((weak)) void player_on_station_change();

#endif
