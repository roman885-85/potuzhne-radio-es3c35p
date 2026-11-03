/*  ---------------------------------------------------------------------------
 *  Доповнення ПОТУЖНОГО РАДІО, яких немає в yoRadio:
 *    - таймер сну (15/30/60/90 хв) із плавним затуханням наприкінці;
 *    - будильник: щодня або в будні, гучність наростає з нуля;
 *    - нічна яркість екрана за розкладом, дотик ненадовго будить;
 *    - батарея (TP4054, дільник 1:2: GPIO9 на ES3C28P, GPIO8 на ES3C35P);
 *    - RGB-світлодіод WS2812 (GPIO42 на ES3C28P, GPIO40 на ES3C35P).
 *
 *  Налаштування живуть в окремому просторі NVS «yoext», а не в сховищі
 *  yoRadio: там свій формат і своя міграція. Записуємо не частіше ніж раз
 *  на три секунди після останньої зміни — серія натискань на «+» дає один
 *  запис, а не десять.
 *
 *  Підсвітку екрана відтепер веде лише цей модуль: він знає і денну
 *  яскравість, і нічну, і те, що екран погашено таймером сну. Плавні
 *  переходи меню й плеєра беруть «повну» яскравість звідси ж, інакше вночі
 *  кожен перехід спалахував би на денну.
 *  ------------------------------------------------------------------------- */
#ifndef yoExtras_h
#define yoExtras_h

#include <Arduino.h>
#include "../core/options.h"     /* плата: виводи світлодіода, батареї, розмір екрана */

#ifndef EXT_BAT_PIN
  #define EXT_BAT_PIN 9          /* ES3C28P; ES3C35P — 8 (myoptions.h) */
#endif
#ifndef EXT_LED_PIN
  #define EXT_LED_PIN 42         /* ES3C28P; ES3C35P — 40 */
#endif
#ifndef EXT_DAC
  #define EXT_DAC 1
#endif

enum extLed_e : uint8_t { LED_OFF = 0, LED_STATUS = 1, LED_MUSIC = 2 };

/*  Калібрування сенсора: позначки стоять за 24 пікселі від країв екрана.
    Типові значення полів tsCal* — самі координати позначок, тож поправка
    тотожна, поки калібрування не зроблено.  */
#define TS_CAL_INSET  24
#define TS_CAL_LO     (TS_CAL_INSET)
#define TS_CAL_XHI    (DSP_W - 1 - TS_CAL_INSET)
#define TS_CAL_YHI    (DSP_H - 1 - TS_CAL_INSET)

struct ExtStore {
  uint8_t  ver;
  uint8_t  alarmOn, alarmH, alarmM, alarmDays;   /* days: 0 щодня, 1 будні */
  uint8_t  nightOn, nightFrom, nightTo;          /* у півгодинах: 0..47 */
  uint8_t  nightLevel;                           /* 0..100, 0 — екран гасне */
  uint8_t  ledMode;
  uint8_t  batSave;                              /* 0 вимк, 1..4 — 10/15/30/60 с без дотиків */
  /*  сторінка «розробник»: нулі — усе ввімкнено, як було  */
  uint8_t  noBat, noIp, noSd;                    /* сховати батарею / IP / функції картки */
  uint8_t  dac;                                  /* аудіовихід: 0 вбудований ES8311, 1 PCM5102A, 2 UDA1334A, 3 MAX98357A */
  uint8_t  favHide;                              /* 1 — рядок обраного на головному не показувати (0 = показувати) */
  /*  Мікрофон. Нуль скрізь — вимкнено: слухати радіо починає лише на прохання.  */
  uint8_t  micOn;                                /* головний вимикач */
  uint8_t  micGain;                              /* 0 — типове; 1..8 = 0..42 дБ */
  uint8_t  micPlay;                              /* слухати й під час звуку (віднімаючи власний звук) */
  uint8_t  clapOn, clapSens, clap2, clap3;       /* хлопки: чутливість 0 середня, 1 низька, 2 висока; дії MicAction */
  uint8_t  knockOn, knockSens, knock2, knock3;   /* стук по корпусу */
  uint8_t  sleepEar, sleepEarMin;                /* таймер сну слухає: тиша N хв (0 — 10) */
  uint8_t  presWake, presOff;                    /* присутність: будити екран; гасити після N хв тиші (0 — ні) */
  /*  Еквалайзер на 10 смуг, дБ (-12..+12), і те, що довкола нього.  */
  int8_t   eq[10];
  uint8_t  eqOn, eqPreset, eqLoud, eqGuard;      /* пресет (0 свій); тонкомпенсація 0/1 м'яка/2 сильна; захист динаміка 0/1 м'який/2 сильний */
  int8_t   eqRoom[10];                           /* поправка під кімнату з заміру мікрофоном, дБ */
  uint8_t  eqRoomOn;
  uint8_t  vbass;                                /* віртуальний бас: 0 вимк, 1..3 сила */
  uint8_t  eqInit;                               /* 1 — типові значення звуку вже виставлено */
  /*  Звуки подій (extras/yoSfx).  */
  uint8_t  sfxInit;                              /* 1 — типові значення вже виставлено */
  uint8_t  sfxOn;                                /* головний вимикач */
  uint8_t  sfxVol;                               /* 0..100 */
  uint16_t sfxMask;                              /* які події озвучувати: біт на подію SfxEvent */
  uint8_t  splashOff;                            /* 1 — без анімованої заставки (0 — показувати) */
  uint8_t  splashVol;                            /* гучність звуку заставки 0..100 (0 — без звуку) */
  uint8_t  splashInit;                           /* 1 — типові значення заставки виставлено */
  uint8_t  menuClassic;                          /* не вживається: старого вигляду немає (поле лишається, щоб не зсунути збережене) */
  uint8_t  otaBeta;                              /* 1 — пропонувати й пробні (попередні) випуски з GitHub */
  uint8_t  sfxEvVol[8];                          /* гучність кожного звуку події, 0..100 % від загальної (індекс — SfxEvent) */
  uint8_t  sfxEvInit;                            /* 1 — гучності подій виставлено */
  uint8_t  dlnaOn;                               /* 1 — бездротова колонка (DLNA) увімкнена */
  uint8_t  dlnaInit;                             /* 1 — типове значення колонки вже виставлено */
  uint8_t  airplayOn;                            /* 1 — колонка AirPlay увімкнена */
  uint8_t  airplayInit;                          /* 1 — типове значення AirPlay уже виставлено */
  int16_t  tsCalXL, tsCalXR, tsCalYT, tsCalYB;   /* калібрування сенсора: виміряні екранні координати чотирьох позначок */
  uint8_t  tsCalInit;                            /* 1 — калібрування вже виставлено (типово — тотожне) */
  uint8_t  lang;                                 /* мова екрана: 0 українська, 1 English (m2/m2lang) */
  uint8_t  dacPwrOld;                            /* не вживається з 02.11.2026 (був дослідний ключ живлення модуля); поле лишається, щоб не зсунути збережене */
  uint8_t  dstMode;                              /* літній час: 0 вимк, 1 сам, за правилом Європи (пояс у налаштуваннях — зимовий) */
  uint8_t  dstInit;                              /* 1 — літній час уже виставлено (перехід зі старого «пояс +3 влітку») */
  uint8_t  noNetCon;                             /* 1 — консоль налагодження по Wi-Fi вимкнена (0 — увімкнена, extras/yoNetCon) */
  uint8_t  extMute;                              /* MUTE зовнішнього підсилювача на EXT_MUTE_PIN: 0 — вивід не чіпаємо,
                                                    1 — тихо низьким рівнем (XSMT, PAM8403), 2 — тихо високим (TPA3116) */
  uint8_t  volMax;                               /* максимальна гучність, % від повної (5..100): гучність на 100 % дає
                                                    стільки; у старому збереженому — 0, тобто 100 (без обмеження) */
  uint8_t  eqRoomDac;                            /* для якого аудіовиходу (dac) зміряно поправку під кімнату; до 02.11.2026
                                                    міряти можна було лише вбудований — у старому збереженому 0 і є він */
  uint8_t  bassOld;                              /* обробка низу: 0 — м'яка (з 03.11.2026), 1 — як була до того; у старому
                                                    збереженому 0 — тобто нова */
};
/*  Зовнішній ЦАП I2S — на вільні виводи роз'ємів; які саме — у myoptions.h для
    кожної плати (ES3C28P: 14/21/2, MUTE IO3; ES3C35P: 45/46/43, MUTE RXD0/IO44).  */
#if EXT_DAC
  #ifndef DAC_BCLK
    #define DAC_BCLK  14
    #define DAC_LRC   21
    #define DAC_DOUT  2
  #endif
  /*  MUTE зовнішнього підсилювача: на ES3C28P — єдиний вільний вивід роз'єму розширення.  */
  #ifndef EXT_MUTE_PIN
    #define EXT_MUTE_PIN   3
  #endif
#else
  #define DAC_BCLK  255
  #define DAC_LRC   255
  #define DAC_DOUT  255
  #define EXT_MUTE_PIN   255
#endif
#ifndef EXT_MUTE_NAME
  #define EXT_MUTE_NAME "IO3"
#endif
#define EXT_STORE_V1  10                          /* розмір першої версії — щоб не губити старі налаштування */

/*  Обране зберігаємо назвою й адресою, а не номером у плейлисті: номер
    зсувається від будь-якої правки списку, адреса — ні.  */
#define FAV_N 6
struct FavItem { char name[48]; char url[160]; };

class YoExtras {
  public:
    ExtStore s;
    FavItem  fav[FAV_N];

    bool     favSetCurrent(uint8_t i);  /* поточну станцію — у клітинку */
    void     favClear(uint8_t i);
    bool     favPlay(uint8_t i);
    int8_t   favPlaying();              /* клітинка станції, що грає, або -1 */

    void begin();
    void loop();                       /* головний цикл, поруч із плеєром */
    void changed();                    /* налаштування змінено — записати згодом */
    void saveNow() { if(_dirty){ _dirty = false; _save(); } }   /* записати зараз (тиша між станціями, сон, перезапуск) */

    /*  таймер сну  */
    void     setSleep(uint16_t minutes);
    uint16_t sleepMinutes() const { return _sleepSet; }
    uint16_t sleepLeft() const;        /* хвилин до кінця, з округленням угору */
    uint32_t sleepLeftSec() const;
    void     sleepTestSec(uint32_t sec); /* для перевірки: таймер у секундах */

    /*  будильник  */
    void     alarmNow();               /* спрацювати негайно (перевірка) */
    void     alarmTestOff(uint16_t sec);  /* перевірка: «вимкнути» й прокинутись будильником через sec секунд */
    bool     alarmRinging() const { return _rampT0 != 0; }
    int32_t  alarmInMin() const;       /* хвилин до найближчого, -1 якщо вимкнено */

    /*  екран  */
    bool     nightActive() const { return _night; }
    uint16_t pwmTarget();              /* яка яскравість має бути зараз, 0..255 */
    void     pwmSet(uint16_t v);       /* записати в пін і запам'ятати */
    uint16_t pwmNow();                 /* одразу виставити ціль (без плавності) */
    bool     touchWake();              /* true — дотик лише розбудив екран */
    bool     screenDim() const { return _dark || _saver || _presDark || _pwmCur == 0; }   /* екран погашено чи пригашено */
    void     screenOff() { _dark = true; }       /* погасити до дотику (жест мікрофона) */
    void     setPresenceDark(bool d) { _presDark = d; }   /* мікрофон: у кімнаті давно тихо */
    bool     presenceDark() const { return _presDark; }
    uint32_t lastTouchMs() const { return _lastTouch; }
    bool     sleepSoon();                  /* таймер сну: почати затихання зараз (у кімнаті тихо) */
    static const char* resetReason();     /* чому радіо перезавантажилось востаннє */
    void     forceNight(int8_t on) { _forceNight = on; }
    void     setBlank(bool b) { _blank = b; }   /* заставка «порожній екран» yoRadio */
    bool     dark() const { return _dark; }
    bool     saverDim() const { return _saver; }  /* пригашено для економії батареї */

    /*  батарея  */
    uint16_t batMv() const { return _batMv; }
    int8_t   batPct() const { return _simPct >= 0 ? _simPct : _batPct; }
    bool     onUsb() const { return _usb; }
    /*  Заряджання — як просив власник: живлення від USB є — заряджається,
        напруга дійшла до 4.2 В — заряджено.  */
    bool     charging() const { return _simChg >= 0 ? _simChg : (_power && !_full); }
    bool     charged() const { return _simChg >= 0 ? false : (_power && _full); }
    bool     onPower() const { return _simChg >= 0 ? _simChg : _power; }
    bool     lowBattery() const { return !s.noBat && _batMv >= 2800 && batPct() >= 0 && batPct() < 10 && !onPower(); }
    void     applyDac();                /* перемкнути виводи I2S на вибраний вихід */
    /*  MUTE зовнішнього підсилювача (EXT_MUTE_PIN). Власник 02.11.2026: «при зупинці — mute, при
        будь-яких діях, коли немає звуку, — mute, при ввімкненні, поки не пішов звук, — mute».
        Тож «звук» — лише поки звук справді йде в I2S: extAmpKick() з виводу плеєра (радіо, картка,
        проповіді, DLNA, AirPlay — усе через yoOutFlush), звуків подій і тонів мікрофона; звук
        перестав іти на EXT_HOLD_MS — «тихо» (перемикання станції, з'єднання, набір буфера,
        провал), зупинка й гучність 0 — «тихо» одразу. extAmp(false) — тихо негайно.  */
    void     extAmp(bool sound);
    void     extAmpKick(bool fromPlayer);   /* звук іде просто зараз (fromPlayer: з плеєра — тоді й гучність не 0) */
    bool     extAmpOn() const { return _extOn; }   /* зараз «звук» */
    void     rateChange();              /* частота I2S от-от зміниться — тихо, поки ЦАП не впіймає такт */
    void     extMuteApply();            /* змінили режим — виставити вивід заново (0 — відпустити) */
    uint16_t batRaw();                 /* разове читання, мВ */
    void     batSim(int8_t pct, int8_t chg);   /* перевірка: підставити рівень, -1 — справжній */

    /*  живлення: 1 — перезавантажити, 2 — вимкнути (глибокий сон до дотику)  */
    void     requestPower(uint8_t mode) { _pwrAt = millis() + 300; _pwrMode = mode; }
    static void earlyBoot();             /* найпершим у setup(): зняти фіксацію виводів після сну */
    static bool wokeByTouch();
    /*  Прокинулось таймером перед будильником (з «вимкнено»): тихий старт —
        без заставки, звуку, підсвітки й автостарту; далі або знову сон, або дзвінок.  */
    static bool wokeForAlarm();
    static bool bootAutoPlay();          /* грати останню станцію після старту (Автостарт, причина старту) */
    static bool alarmQuiet();            /* зараз тихе чекання будильника (екран темний, радіо «вимкнене») */
    uint32_t lowBatBeat() const { return _lowBeat; }   /* росте з кожним попередженням про батарею (раз на хвилину) */

    /*  світлодіод  */
    void     ledTest(uint8_t r, uint8_t g, uint8_t b, uint16_t ms);

    /*  Wi-Fi: до якої мережі просили підключитись перед перезавантаженням.
        Після старту перевіряємо, чи вийшло, і якщо ні — кажемо про це.  */
    void     wifiPending(const char* ssid);
    void     _wifiFileGuard(uint32_t now);   /* файл мереж порожній — відновити з пам'яті */
    char     wifiFail[33] = {0};         /* не вдалося підключитись до цієї */

  private:
    volatile bool _extOn = false;       /* MUTE зовнішнього підсилювача: зараз «звук» */
    volatile uint32_t _extAt = 0;       /* коли востаннє йшов звук (extAmpKick) */
    volatile uint32_t _extHold = 0;     /* до цієї миті MUTE не знімати: ЦАП ловить новий такт (rateChange) */
    void          _extAmpLoop(uint32_t now);   /* звук не йде EXT_HOLD_MS — тихо */
    bool          _extSet = false;      /* вивід уже виставлено (перший запис — завжди) */
    bool     _dirty = false;
    uint32_t _dirtyMs = 0;
    void     _load();
    void     _save();

    uint16_t _sleepSet = 0;            /* хвилин, 0 — вимкнено */
    uint32_t _sleepEnd = 0;            /* millis() кінця */
    bool     _sleepFading = false;
    void     _sleepLoop(uint32_t now);

    uint32_t _rampT0 = 0;              /* наростання будильника триває */
    uint8_t  _rampTo = 0;
    int16_t  _lastAlarmKey = -1;       /* хвилина доби, коли вже дзвонили */
    void     _alarmLoop(uint32_t now);
    void     _alarmWakeLoop(uint32_t now);
    void     _lowBatLoop(uint32_t now);
    volatile uint32_t _lowBeat = 0;
    uint32_t _lowSince = 0, _lowNext = 0, _attnUntil = 0;
    int64_t  _alarmSecs() const;       /* секунд до найближчого будильника за годинником системи; -1 — нема */
    int32_t  _alarmPassedSecs() const; /* скільки секунд тому був сьогоднішній будильник (якщо сьогодні день будильника), -1 */
    void     _alarmStart();

    bool     _night = false;
    int8_t   _forceNight = -1;
    bool     _dark = false;            /* погашено таймером сну до дотику */
    bool     _blank = false;
    uint32_t _wakeUntil = 0;           /* дотик уночі — денна яскравість до */
    uint32_t _lastTouch = 0;           /* останній дотик — для економії батареї */
    bool     _saver = false;
    volatile bool _presDark = false;
    uint16_t _pwmCur = 0xFFFF;
    uint32_t _pwmTick = 0;
    void     _screenLoop(uint32_t now);

    uint16_t _batMv = 0;
    int8_t   _batPct = -1;
    bool     _usb = false;
    uint32_t _batTick = 0;
    bool     _charging = false;         /* здогадка з напруги: зарядний блок без даних */
    bool     _power = false, _full = false;
    uint16_t _batPrev = 0;
    uint16_t _batMin[6] = {0};
    uint8_t  _batMinN = 0;
    uint32_t _batMinT = 0;
    int8_t   _simPct = -1, _simChg = -1;
    uint32_t _batAcc = 0;
    uint8_t  _batN = 0;
    void     _batLoop(uint32_t now);

    char     _wpend[33] = {0};
    void     _wifiCheck(uint32_t now);
    volatile uint8_t _pwrMode = 0;
    uint32_t _pwrAt = 0;
    void     _powerOff();
    void     _sleepUntilTouch();          /* ES3C35P: легкий сон до дотику або кнопки BOOT (TS_INT — не RTC-вивід) */
    void     _armAlarmWake();          /* перед сном: пробудження таймером до будильника */
    int64_t  _alarmSleepSecs();        /* скільки спати до пробудження перед будильником (-1 — будильника нема) */
    uint32_t _ledTick = 0;
    uint32_t _ledTestUntil = 0;
    uint8_t  _ledR = 1, _ledG = 1, _ledB = 1;   /* свідомо не нуль: перший запис відбудеться */
    float    _ledEnv = 0.0f;
    void     _ledLoop(uint32_t now);
    void     _led(uint8_t r, uint8_t g, uint8_t b);
};

extern YoExtras extras;

#endif
