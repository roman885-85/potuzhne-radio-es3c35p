/*  =============================================================================
 *  ПОТУЖНЕ РАДІО — дві плати LCDwiki з однієї вихідної теки.
 *
 *  ES3C28P / ES3N28P — 2,8" IPS 320x240, ILI9341V (SPI) + FT6336, ES8311 + FM8002E.
 *  ES3C35P / ES3C35P-NS — 3,5" IPS 320x480, ST77922 (QSPI) із вбудованим сенсором
 *                         (I2C 0x55), ES8311 + SC8002B. Працює в ландшафті 480x320.
 *
 *  Яку плату збирати, каже firmware/rebuild.sh: «--board es3c35p» додає до збірки
 *  -DPR_BOARD_ES3C35P=1, без нього — ES3C28P (як і раніше).
 *  Обидві: ESP32-S3 N16R8 (16 MB QSPI flash, 8 MB OPI PSRAM), USB-CDC без моста.
 *  =============================================================================
 */
#ifndef myoptions_h
#define myoptions_h

#if defined(PR_BOARD_ES3C35P)

/*  =============================================================================
 *  ES3C35P: виводи — зі схеми «ESP32-S3原理图.pdf» (docs/ES3C35P) і прикладів
 *  LCDwiki (ydedox/st77922, xiaozhi-esp32 boards/lcdwiki-es3c35p).
 *  Вільні виводи: 45, 46 (роз'єм розширення), 43/44 (UART0), 38/39 (I2C, спільна шина).
 *  ========================================================================== */
#define PR_BOARD          "ES3C35P"
#define PR_REPO           "roman885-85/potuzhne-radio-es3c35p"   /* свій репозиторій: випуски, звідки радіо оновлюється */
#define PR_BOARD_NAME     "ES3C35P: ESP32-S3, екран 3,5\u2033 ST77922 (QSPI) із сенсором, кодек ES8311, картка пам'яті, 16 МБ флеш, 8 МБ PSRAM"

/*  ----------  ДИСПЛЕЙ  ST77922 320x480 по QSPI (у ландшафті 480x320)  ------- */
/*  Апаратного повороту (MV) панель у режимі QSPI не має — радіо повертає
    картинку саме (displays/displayST77922.cpp). Скидання екрана заведене на
    EN разом із процесором.                                                    */
#define DSP_MODEL         DSP_ST77922
#define DSP_W             480
#define DSP_H             320
#define TFT_CS            10
#define TFT_DC            -1
#define TFT_RST           -1
#define LCD_QSPI_SCK      12
#define LCD_QSPI_D0       11
#define LCD_QSPI_D1       13
#define LCD_QSPI_D2       14
#define LCD_QSPI_D3       9
#define LCD_TE            42        /* сигнал кадрової розгортки (поки не використовується) */
#define BRIGHTNESS_PIN    41        /* підсвітка через BSS138, високий рівень = увімкнена */
#define DSP_INVERT_TITLE  false
#define DSP_INVERT_BASE   true      /* у послідовності виробника 0x21 (інверсія ВКЛ) */

/*  ----------  ШИНА I2C (спільна: сенсор ST77922 0x55 + ES8311 0x18 + роз'єм I2C) */
#define I2C_SDA           38
#define I2C_SCL           39

/*  ----------  СЕНСОРНИЙ ЕКРАН — вбудований у ST77922  -------------------------- */
#define TS_MODEL          TS_MODEL_ST77922
#define TS_SDA            38
#define TS_SCL            39
#define TS_INT            47        /* не RTC-вивід: будити з глибокого сну не може */
#define TS_RST            48

/*  ----------  ЗВУК: I2S -> ES8311 -> SC8002B -> динамік  --------------------- */
#define I2S_DOUT          15        /* ESP32 -> кодек (DSDIN; на схемі I2S_DI) */
#define I2S_BCLK          18
#define I2S_LRC           21
#define I2S_MCLK          17
#define I2S_DIN           16        /* кодек -> ESP32 (ASDOUT; на схемі I2S_DO): мікрофон */
#define I2S_ES8311        true
#define ES8311_ADDR       0x18
#define ES8311_VOLUME     75

/*  Підсилювач SC8002B: SHUTDOWN підтягнутий до 3,3 В, GPIO1 НИЗЬКИМ рівнем вмикає.  */
#define MUTE_PIN          1
#define MUTE_VAL          HIGH
#define MUTE_LOCK         false

/*  ----------  SD-КАРТКА (SDIO 4 біти)  ---------------------------------------- */
#define SD_SDMMC          true
#define SDMMC_CLK         5
#define SDMMC_CMD         4
#define SDMMC_D0          6
#define SDMMC_D1          7
#define SDMMC_D2          2
#define SDMMC_D3          3

/*  ----------  КНОПКИ — НЕМАЄ  ----------------------------------------------- */
/*  Рішення власника 25.10.2026: у роботі радіо не бере участі жодна кнопка —
    лише сенсор. BOOT (GPIO0) лишається тільки для прошивання; радіо її не читає
    і нею не будиться.                                                         */
#define BTN_CENTER        255

/*  ----------  СВІТЛОДІОД І БАТАРЕЯ  ------------------------------------------ */
#define USE_BUILTIN_LED   false
#define LED_BUILTIN_S3    255
#define EXT_LED_PIN       40        /* WS2812B (XL-5050RGBC), живлення 5 В */
#define EXT_BAT_PIN       8         /* дільник 100К/100К — множимо на 2, як на ES3C28P */

/*  Зовнішній ЦАП I2S (PCM5100 / PCM5102A, UDA1334A, MAX98357A) — на штатних
    роз'ємах 4P 1,25 мм на звороті плати (схема ES3C35P, «外部接口»):
      Expand (P3): 1 — 3V3, 2 — GND, 3 — IO46 → LCK, 4 — IO45 → BCK;
      UART   (P2): 1 — +5 В → VIN, 2 — GND, 3 — TXD0 (IO43) → DIN, 4 — RXD0 (IO44) — ключ живлення.
    Виводи ES3C28P (2, 3, 14, 21) тут зайняті карткою, екраном і звуком.
    IO45/IO46 — стапи завантаження (за замовчуванням у нулі), модулю PCM51xx це
    не заважає: його входи без підтяжок. Власник, 25.10.2026: «к штатным
    разъемам нужно подключить pcm 5100».  */
#define EXT_DAC           1
#define DAC_BCLK          45
#define DAC_LRC           46
#define DAC_DOUT          43
#define DAC_PWR           44
#define DAC_PWR_NAME      "RXD0"

/*  ----------  ПРОЧЕЕ  ------------------------------------------------------ */
#define L10N_LANGUAGE     RU
#define ROTATE_90         false
#define SD_AUTOPLAY       true
#define VS1053_CS         255
#define WATCHDOG_INTERVAL 8
#define YO_DEBUG          true

#else  /* ============================ ES3C28P ============================ */

#define PR_BOARD          "ES3C28P"
#define PR_REPO           "roman885-85/potuzhne-radio"
#define PR_BOARD_NAME     "ES3C28P: ESP32-S3, екран 2,8\u2033 ILI9341 із сенсором, кодек ES8311, картка пам'яті, 16 МБ флеш, 8 МБ PSRAM"
#define DSP_W             320
#define DSP_H             240
#define EXT_LED_PIN       42        /* WS2812 */
#define EXT_BAT_PIN       9         /* TP4054, дільник 1:2 */
#define EXT_DAC           1         /* зовнішній ЦАП на роз'ємі Expand (GPIO 14/21/2, живлення IO3) */
#define DAC_BCLK          14
#define DAC_LRC           21
#define DAC_DOUT          2
#define DAC_PWR           3
#define DAC_PWR_NAME      "IO3"


/*  ----------  ДИСПЛЕЙ  ILI9341V 240x320 (работает в ландшафте 320x240)  ----- */
#define DSP_MODEL         DSP_ILI9341
#define TFT_CS            10        // LCD_CS
#define TFT_DC            46        // LCD_DC   (1 = data, 0 = command)
#define TFT_RST           -1        // сброс дисплея заведён на CHIP_PU
#define DSP_HSPI          false     // FSPI по умолчанию: SCLK 12, MOSI 11, MISO 13
#define BRIGHTNESS_PIN    45        // подсветка, высокий уровень = включена
#define DSP_INVERT_TITLE  false
/*  Панель IPS: в фирменной инициализации стоит команда 0x21 (инверсия ВКЛ).
    Без этого картинка идёт негативом.                                       */
#define DSP_INVERT_BASE   true

/*  ----------  ШИНА I2C (общая: FT6336 + ES8311)  --------------------------- */
#define I2C_SDA           16
#define I2C_SCL           15

/*  ----------  СЕНСОРНЫЙ ЭКРАН  FT6336  ------------------------------------- */
#define TS_MODEL          TS_MODEL_FT6336
#define TS_SDA            16
#define TS_SCL            15
#define TS_INT            17
#define TS_RST            18

/*  ----------  ЗВУК: I2S -> ES8311 -> FM8002E -> динамик  ------------------- */
#define I2S_DOUT          8         // ESP32 -> кодек (DSDIN)
#define I2S_BCLK          5         // SCLK
#define I2S_LRC           7         // LRCK / WS
#define I2S_MCLK          4         // MCLK, обязателен для ES8311
#define I2S_DIN           6         // кодек -> ESP32 (ASDOUT): вбудований мікрофон
#define I2S_ES8311        true      // включить драйвер кодека
#define ES8311_ADDR       0x18      // вывод CE кодека притянут к земле
#define ES8311_VOLUME     75        // 75 = 0 dB. Выше 75 — цифровое усиление

/*  Разрешение усилителя FM8002E: GPIO1, НИЗКИЙ уровень = усилитель включён.
 *  yoRadio держит на MUTE_PIN значение MUTE_VAL, пока плеер остановлен.        */
#define MUTE_PIN          1
#define MUTE_VAL          HIGH
#define MUTE_LOCK         false

/*  ----------  SD-КАРТА (SDIO 4 бит, не SPI)  ------------------------------- */
#define SD_SDMMC          true
#define SDMMC_CLK         38
#define SDMMC_CMD         40
#define SDMMC_D0          39
#define SDMMC_D1          41
#define SDMMC_D2          48
#define SDMMC_D3          47

/*  ----------  КНОПКА  ------------------------------------------------------ */
/*  GPIO0 — это кнопка BOOT. Коротко — пуск/стоп, долго — переход в список.
 *  Во время включения питания её держать нельзя: плата уйдёт в режим прошивки. */
#define BTN_CENTER        0
#define BTN_INTERNALPULLUP true

/*  ----------  СВЕТОДИОД  --------------------------------------------------- */
/*  На плате только адресный WS2812 (GPIO42), обычным digitalWrite не мигает,
 *  а LED_BUILTIN у «ESP32S3 Dev Module» = 48 и совпал бы с SD_D2.              */
#define USE_BUILTIN_LED   false
#define LED_BUILTIN_S3    255

/*  ----------  ПРОЧЕЕ  ------------------------------------------------------ */
#define L10N_LANGUAGE     RU
#define ROTATE_90         false
#define SD_AUTOPLAY       true
#define VS1053_CS         255
#define WATCHDOG_INTERVAL 8

/*  Діагностика через послідовний порт: знімок екрана, керування плеєром,
    стан картки. Потрібна, поки пристрій перевіряється без людини поруч.   */
#define YO_DEBUG          true

/*  ----------  НАЗВА ПРИСТРОЮ  --------------------------------------------- */
/*  SSID і mDNS лишаємо латиницею: кирилиця в іменах мереж і hostname
    підтримується не всюди й ламає доступ з частини пристроїв.               */


#endif  /* плата */

#endif
