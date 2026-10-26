#include "Arduino.h"
#include "core/options.h"
#include "core/config.h"
#include "pluginsManager/pluginsManager.h"
#include "core/telnet.h"
#include "core/player.h"
#include "core/display.h"
#include "core/network.h"
#include "core/netserver.h"
#include "core/controls.h"
#include "core/optionschecker.h"
#include "menu/yoDebug.h"
#include "core/timekeeper.h"
#include "extras/yoExtras.h"
#include "menu/yoMenu.h"
#include "extras/yoMic.h"
#include "extras/yoDsp.h"
#include "extras/yoSfx.h"
#include "extras/yoOta.h"
#include "extras/yoHang.h"
#include "extras/yoDlna.h"
#include "extras/yoAirplay.h"
#ifdef USE_NEXTION
#include "displays/nextion.h"
#endif

#if USE_OTA
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
#include <NetworkUdp.h>
#else
#include <WiFiUdp.h>
#endif
#include <ArduinoOTA.h>
#endif

#if DSP_HSPI || TS_HSPI || VS_HSPI
SPIClass  SPI2(HSPI);
#endif

extern __attribute__((weak)) void yoradio_on_setup();

#if USE_OTA
void setupOTA(){
  if(strlen(config.store.mdnsname)>0)
    ArduinoOTA.setHostname(config.store.mdnsname);
#ifdef OTA_PASS
  ArduinoOTA.setPassword(OTA_PASS);
#endif
  ArduinoOTA
    .onStart([]() {
      player.sendCommand({PR_STOP, 0});
      display.putRequest(NEWMODE, UPDATING);
      telnet.printf("Start OTA updating %s\n", ArduinoOTA.getCommand() == U_FLASH?"firmware":"filesystem");
    })
    .onEnd([]() {
      telnet.printf("\nEnd OTA update, Rebooting...\n");
      ESP.restart();
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      telnet.printf("Progress OTA: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      telnet.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) {
        telnet.printf("Auth Failed\n");
      } else if (error == OTA_BEGIN_ERROR) {
        telnet.printf("Begin Failed\n");
      } else if (error == OTA_CONNECT_ERROR) {
        telnet.printf("Connect Failed\n");
      } else if (error == OTA_RECEIVE_ERROR) {
        telnet.printf("Receive Failed\n");
      } else if (error == OTA_END_ERROR) {
        telnet.printf("End Failed\n");
      }
    });
  ArduinoOTA.begin();
}
#endif

void setup() {
  /*  Звук веде цей цикл — це найсрочніша робота в радіо (див. таблицю
      пріоритетів нижче). Ставимо одразу: інакше при старті без мережі
      налаштування нижче не виконається.  */
  vTaskPrioritySet(nullptr, 4);
  YoExtras::earlyBoot();      /* після сну «вимкнено»: зняти фіксацію виводів, доки їх ніхто не чіпав */
  Serial.begin(115200);
  YoHang::boot();             /* чи не зависання спричинило цей старт — до того, як будь-що перезапише запис */
  if(REAL_LEDBUILTIN!=255) pinMode(REAL_LEDBUILTIN, OUTPUT);
  if (yoradio_on_setup) yoradio_on_setup();
  pm.on_setup();
  config.init();
  YoSfx::mount();           /* розділ ресурсів: заставка потрібна вже екрану завантаження */
  display.init();
  player.init();
  extras.begin();           /* таймер сну, будильник, ніч, батарея, світлодіод */
  Serial.printf("##[BOOT]#\tостаннє перезавантаження: %s\n", YoExtras::resetReason());
  mic.begin();              /* вбудований мікрофон: задача слухає, лише коли його ввімкнули */
  sfx.begin();              /* звуки подій: розділ ресурсів і своя задача виводу */
  if(!YoExtras::wokeForAlarm()) sfx.play(SFX_START);   /* перед будильником — тихо */
  network.begin();
  if (network.status != CONNECTED && network.status!=SDREADY) {
    netserver.begin();
    initControls();
    display.putRequest(DSP_START);
    while(!display.ready()) delay(10);
    YoHang::begin();
    return;
  }
  if(SDC_CS!=255) {
    display.putRequest(WAITFORSD, 0);
    Serial.print("##[BOOT]#\tSD search\t");
  }
  config.initPlaylistMode();
  netserver.begin();
  telnet.begin();
  initControls();
  display.putRequest(DSP_START);
  while(!display.ready()) delay(10);
  #ifdef MQTT_ROOT_TOPIC
    mqttInit();
  #endif
  #if USE_OTA
    setupOTA();
  #endif
  dlna.begin();               /* бездротова колонка (DLNA), якщо увімкнена */
  airplay.begin();            /* і AirPlay */
  if (config.getMode()==PM_SDCARD) player.initHeaders(config.station.url);
  player.lockOutput=false;
  if (config.store.smartstart == 1 && !YoExtras::wokeForAlarm()) {     /* будильник сам увімкне станцію */
    player.sendCommand({PR_PLAY, config.lastStation()});
    Serial.printf("##BOOT#\tграти після ввімкнення: станція %u\n", (unsigned)config.lastStation());
  }else Serial.printf("##BOOT#\tпісля ввімкнення не граю (smartstart=%u, будильник=%d)\n", (unsigned)config.store.smartstart, YoExtras::wokeForAlarm() ? 1 : 0);
  pm.on_end_setup();
  YoHang::begin();            /* сторож зависань: головний цикл і задача екрана */
}

#ifdef YO_DEBUG
/*  Хто саме тримає головний цикл: дотик, звук і меню живуть тут же, і будь-яка
    затримка довша за глибину буфера звуку чутна й видна. Нічого не друкуємо:
    запис у USB-порт сам блокує цикл, коли з того боку ніхто не читає, — лише
    запам'ятовуємо найдовший крок, а показує його команда «page».  */
char     yoSlowWhat[40] = {0};
uint32_t yoSlowMs = 0;
/*  Як часто взагалі крутиться цикл: дотик опитується раз на оберт, тож
    рідкий цикл — це і є «тормоза», навіть коли жоден крок не довгий.  */
uint32_t yoLoopN = 0, yoLoopMax = 0, yoLoopFrom = 0;
/*  aunet: те саме, але за кожну секунду окремо — щоб провал звуку було з чим зіставити.  */
static char     yoSecWhat[40] = {0};
static uint32_t yoSecMs = 0, yoSecLoop = 0, yoSecPlUs = 0;
volatile bool   yoAuNet = false;
/*  Останній крок довший за 40 мс — його друкує повідомлення про провал звуку (Audio.cpp).  */
char     yoLongWhat[40] = {0};
uint32_t yoLongMs = 0, yoLongAt = 0;
static inline void yoSlow(const char* what, uint32_t t0){
  uint32_t d = millis() - t0;
  if(d > 40){ yoLongMs = d; yoLongAt = millis(); strlcpy(yoLongWhat, what, sizeof(yoLongWhat)); }
  if(d > yoSecMs){ yoSecMs = d; strlcpy(yoSecWhat, what, sizeof(yoSecWhat)); }
  if(d > 100 && d > yoSlowMs){ yoSlowMs = d; strlcpy(yoSlowWhat, what, sizeof(yoSlowWhat)); }
}
/*  Щосекунди (лише після команди aunet): скільки прийшло з мережі, скільки лежить у буфері
    потоку, скільки відліків пішло в I2S і скільки часу цикл стояв у i2s_write (це добре:
    буфер DMA повний), провали і найдовший крок циклу за ту саму секунду.  */
static void yoAuNetTick(){
  extern volatile uint32_t yoNetRx, yoNetAvMax, yoI2sWaitUs, yoAuUnderN, yoAuSamples;
  static uint32_t t0 = 0, s0 = 0, u0 = 0;
  const uint32_t now = millis();
  if(now - t0 < 1000) return;
  if(yoAuNet && t0){
    const uint32_t dt = now - t0;
    Serial.printf("AUNET %u Б/с, буфер %u КБ, відліків %u/с, провалів +%u, I2S чекав %u мс, плеєр %u мс, avail≤%u, цикл≤%u мс, крок %s %u мс, RSSI %d\n",
                  (unsigned)((uint64_t)yoNetRx * 1000 / dt), (unsigned)(player.inBufferFilled() / 1024),
                  (unsigned)((uint64_t)(yoAuSamples - s0) * 1000 / dt), (unsigned)(yoAuUnderN - u0),
                  (unsigned)(yoI2sWaitUs / 1000), (unsigned)(yoSecPlUs / 1000), (unsigned)yoNetAvMax,
                  (unsigned)yoSecLoop, yoSecWhat[0] ? yoSecWhat : "-", (unsigned)yoSecMs, (int)WiFi.RSSI());
  }
  yoNetRx = 0; yoNetAvMax = 0; yoI2sWaitUs = 0; s0 = yoAuSamples; u0 = yoAuUnderN;
  yoSecMs = 0; yoSecWhat[0] = 0; yoSecLoop = 0; yoSecPlUs = 0;
  t0 = now;
}
  #define STEP(call) { uint32_t _t0 = millis(); call; yoSlow(#call, _t0); }
#else
  #define STEP(call) call
#endif

/*  ============================ ПРІОРИТЕТИ ЗАДАЧ ============================
    Хто кому має поступатися. Правило просте: спершу те, у чого є строк
    (звук і мережеві пакети), потім те, що видно оком (екран), і лише потім
    фонові завантаження. Понад 10 не піднімаємось — вище живуть мережевий
    стек (18) і Wi-Fi (23), їх чіпати не можна.

      ядро 1 (звук)            ядро 0 (мережа й екран)
      ---------------------    -------------------------------
      4  головний цикл*        5  airplay (пакети звуку)
      3  sfx (звуки подій)     4  DspTask (екран)
      1  otaChk/otaIns/apKey   3  mic (слух; на час тонів — 6)
                               1  dlna, stlist, sermons, cover, logo, пошук Wi-Fi
                              10  hangWd (сторож)

    * головний цикл декодує потік і віддає відліки в I2S — це найсрочніша
      робота в радіо, а працювала вона з пріоритетом 1, нижче за звуки подій
      і нарівні із завантаженням оновлення. Через це звук сипався саме тоді,
      коли радіо ще чимось зайняте.
    ========================================================================= */
void loop() {
  yoHbLoop++;
#ifdef YO_DEBUG
  {
    static uint32_t prev = 0;
    uint32_t now = millis();
    if(!yoLoopFrom) yoLoopFrom = now;
    if(prev){ uint32_t d = now - prev; if(d > yoLoopMax) yoLoopMax = d; if(d > yoSecLoop) yoSecLoop = d; }
    prev = now; yoLoopN++;
  }
  yodbgLoop();
  yoAuNetTick();
#endif
  STEP(timekeeper.loop1());
  STEP(config.eepromLoop());      /* відкладений запис налаштувань */
  STEP(network.loop());           /* мережа зникла — повертаємось у неї без зупинок */
  STEP(telnet.loop());
  /*  Плеєр крутимо завжди: навіть без мережі його чергу треба розгрібати,
      інакше вона забивається, а той, хто в неї пише (задача екрана), стоїть
      на кожній посилці. Саме через це радіо «тормозило» без мережі.  */
#ifdef YO_DEBUG
  { const uint32_t _u = micros(); STEP(player.loop()); yoSecPlUs += micros() - _u; }
#else
  STEP(player.loop());
#endif
  if (network.status == CONNECTED || network.status==SDREADY) {
#if USE_OTA
    STEP(ArduinoOTA.handle());
#endif
  }
  STEP(loopControls());
#ifdef USE_YOMENU
  STEP(ota.loop());               /* оновлення з GitHub: автоперевірка двічі на добу */
  STEP(airplay.loop());           /* AirPlay: ключ і оголошення в мережі */
  STEP(yomenu.wifiTick());        /* пошук мереж — тут, а не в задачі дисплея */
  STEP(yoDsp.roomTick());         /* налаштування під кімнату: тони, замір, поправка */
  STEP(mic.loop());               /* мікрофон: дії на хлопки й стук, присутність, сон */
#endif
  STEP(extras.loop());
  #ifdef NETSERVER_LOOP1
  STEP(netserver.loop());
  #endif
}

#include "core/audiohandlers.h"
