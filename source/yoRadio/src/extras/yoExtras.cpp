#include "yoExtras.h"
#include "yoATrace.h"
#include "yoSfx.h"
#include <Preferences.h>
#include <SPIFFS.h>
#include "yoRecorder.h"
#include "yoSermons.h"
#include "yoLogos.h"
#include "yoWebApi.h"
#include "yoDsp.h"
#include "../core/options.h"
#include "../core/config.h"
#include "../core/player.h"
#include "../core/network.h"
#include "../core/display.h"
#include "../menu/yoMenu.h"
#include "../m2/m2ui.h"
#include "../m2/m2lang.h"
#include <WiFi.h>
#include <HWCDC.h>
#include <Wire.h>
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_sntp.h"
#include "esp_heap_caps.h"
#include "mbedtls/platform.h"
#include "driver/rtc_io.h"
#include "driver/gpio.h"
#include "../ES8311/yoES8311.h"
#include "../core/sdmanager.h"
#include "esp_timer.h"
#if TS_MODEL==TS_MODEL_ST77922
  #include "../ST77922_Touch/yoST77922Touch.h"
#endif

YoExtras extras;

#define EXT_VER          1
#define SLEEP_FADE_MS    20000UL     /* затухання наприкінці таймера сну */
#define ALARM_RAMP_MS    30000UL     /* наростання будильника */
#define ALARM_WAIT_MS    25000UL     /* скільки чекати, поки станція заграє */
#define ALARM_MIN_VOL    40          /* тихіше будильник не буває */
#define WAKE_MS          15000UL     /* дотик уночі — денна яскравість на стільки */
#define LED_MAX          28          /* WS2812 сліпить уже на 30 з 255 */

/*  ---------- налаштування ---------- */

void YoExtras::_load(){
  memset(&s, 0, sizeof(s));
  Preferences p;
  bool ok = false;
  if(p.begin("yoext", true)){
    /*  Старіший запис коротший (без нових полів) — читаємо, що є, нові
        лишаються нулями. Інакше будильник і ніч скидались би з кожним
        новим полем.  */
    size_t ln = p.getBytesLength("s");
    if(ln >= EXT_STORE_V1 && ln <= sizeof(s)){
      p.getBytes("s", &s, ln);
      ok = (s.ver == EXT_VER);
    }
    p.end();
  }
  if(!ok){
    memset(&s, 0, sizeof(s));
    s.ver = EXT_VER;
    s.alarmOn = 0; s.alarmH = 7; s.alarmM = 0; s.alarmDays = 0;
    s.nightOn = 0; s.nightFrom = 44; s.nightTo = 14; s.nightLevel = 10;   /* 22:00..07:00 */
    s.ledMode = LED_STATUS;
  }
  if(s.alarmH > 23) s.alarmH = 7;
  if(s.alarmM > 59) s.alarmM = 0;
  if(s.nightFrom > 47) s.nightFrom = 44;
  if(s.nightTo > 47) s.nightTo = 14;
  if(s.nightLevel > 100) s.nightLevel = 10;
  if(s.ledMode > LED_MUSIC) s.ledMode = LED_STATUS;
  if(s.batSave > 4) s.batSave = 0;
  if(s.dac > 3) s.dac = 0;
  /*  Звук: при першому запуску цієї версії — захист динаміка ввімкнено, а
      три повзунки yoRadio переносяться в десять смуг.  */
  if(!s.sfxInit){
    s.sfxInit = 1; s.sfxOn = 1; s.sfxVol = 60;
    s.sfxMask = (1 << 0) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6);   /* усе, крім дотику (YoSfx::DEFAULT_MASK) */
  }
  if(s.sfxVol > 100) s.sfxVol = 60;
  if(!s.splashInit){ s.splashInit = 1; s.splashVol = 70; }
  if(!s.sfxEvInit){ s.sfxEvInit = 1; for(uint8_t i = 0; i < sizeof(s.sfxEvVol); i++) s.sfxEvVol[i] = 100; }
  if(!s.dlnaInit){ s.dlnaInit = 1; s.dlnaOn = 1; }      /* бездротова колонка — одразу увімкнена */
  if(!s.airplayInit){ s.airplayInit = 1; s.airplayOn = 1; }   /* і AirPlay теж */
  if(!s.tsCalInit){ s.tsCalInit = 1; s.tsCalXL = TS_CAL_LO; s.tsCalXR = TS_CAL_XHI; s.tsCalYT = TS_CAL_LO; s.tsCalYB = TS_CAL_YHI; }
  for(uint8_t i = 0; i < sizeof(s.sfxEvVol); i++) if(s.sfxEvVol[i] > 100) s.sfxEvVol[i] = 100;
  if(s.splashVol > 100) s.splashVol = 70;
  if(!s.eqInit){
    s.eqInit = 1; s.eqOn = 1; s.eqGuard = 1; s.eqLoud = 0; s.vbass = 0; s.eqPreset = 0;
    int8_t b = config.store.bass, m = config.store.middle, t = config.store.trebble;
    if(b > 6) b = 6;                       /* старі фільтри підіймали не більше ніж на 6 дБ */
    if(m > 6) m = 6;
    if(t > 6) t = 6;
    int tt[10] = { b, b, b, b / 2, 0, m / 3, m * 2 / 3, m + t / 3, t, t };
    for(uint8_t i = 0; i < 10; i++) s.eq[i] = (int8_t)(tt[i] > 12 ? 12 : tt[i] < -12 ? -12 : tt[i]);
  }
  for(uint8_t i = 0; i < 10; i++){
    if(s.eq[i] > 12) s.eq[i] = 12;
    if(s.eq[i] < -12) s.eq[i] = -12;
    if(s.eqRoom[i] > 12 || s.eqRoom[i] < -12) s.eqRoom[i] = 0;
  }
  if(s.eqPreset >= 8) s.eqPreset = 0;
  if(s.eqLoud > 2) s.eqLoud = 0;
  if(s.eqGuard > 2) s.eqGuard = 1;
  if(s.vbass > 3) s.vbass = 0;
  if(s.lang >= m2::LANG_N) s.lang = m2::LANG_UK;
  s.dacPwrOld = 0;                       /* ключ живлення модуля прибрано (02.11.2026): вивід тепер MUTE */
  if(s.extMute > 2) s.extMute = 0;
  if(s.volMax < 5 || s.volMax > 100) s.volMax = 100;
  if(s.bassOld > 1) s.bassOld = 0;
  /*  Літній час (26.10.2026, прохання власника): типово — сам, за правилом Європи, а пояс
      у налаштуваннях тепер зимовий. Хто влітку ставив +3 (Київ), отримує +2 і далі
      перехід сам: улітку однаково +3, узимку — +2, як і має бути.  */
  if(!s.dstInit){
    s.dstInit = 1;
    const int16_t m = config.store.tzHour * 60 + config.store.tzMin;
    if(m >= 0 && m <= 180){ s.dstMode = 1; if(m == 180) config.setTimezone(2, 0); }
    _dirty = true; _dirtyMs = millis();
  }
  if(s.dstMode > 1) s.dstMode = 0;
#if !EXT_DAC
  s.dac = 0; s.extMute = 0;             /* на цій платі зовнішнього ЦАП немає */
#endif
  m2::langSet(s.lang);                   /* мова екрана — до першого малювання */
  config.applyTz();                      /* пояс із літнім часом — щойно відомий режим */
  yoDsp.changed();

  memset(fav, 0, sizeof(fav));
  if(p.begin("yoext", true)){
    if(p.getBytesLength("fav") == sizeof(fav)) p.getBytes("fav", fav, sizeof(fav));
    p.end();
  }
  for(uint8_t i = 0; i < FAV_N; i++){ fav[i].name[sizeof(fav[i].name)-1] = 0; fav[i].url[sizeof(fav[i].url)-1] = 0; }
}

void YoExtras::_save(){
  Preferences p;
  if(!p.begin("yoext", false)) return;
  /*  Пишемо лише справді інше: NVS однаково переписала б сторінку.  */
  ExtStore old;
  if(p.getBytesLength("s") != sizeof(s) || (p.getBytes("s", &old, sizeof(old)), memcmp(&old, &s, sizeof(s)) != 0))
    p.putBytes("s", &s, sizeof(s));
  FavItem* of = (FavItem*)malloc(sizeof(fav));
  if(of){
    if(p.getBytesLength("fav") != sizeof(fav) || (p.getBytes("fav", of, sizeof(fav)), memcmp(of, fav, sizeof(fav)) != 0))
      p.putBytes("fav", fav, sizeof(fav));
    free(of);
  }
  p.end();
}

/*  ---------- обране ---------- */

bool YoExtras::favSetCurrent(uint8_t i){
  if(i >= FAV_N || config.getMode() != PM_WEB || !config.station.url[0] || !config.station.name[0]) return false;
  strlcpy(fav[i].url, config.station.url, sizeof(fav[i].url));
  strlcpy(fav[i].name, config.station.name, sizeof(fav[i].name));
  utf8FixTail(fav[i].name);              /* не лишаємо півлітери, якщо назву обрізало */
  changed();
  return true;
}

void YoExtras::favClear(uint8_t i){
  if(i >= FAV_N) return;
  memset(&fav[i], 0, sizeof(fav[i]));
  changed();
}

int8_t YoExtras::favPlaying(){
  if(config.getMode() != PM_WEB || player.status() != PLAYING) return -1;
  for(uint8_t i = 0; i < FAV_N; i++)
    if(fav[i].url[0] && !strcmp(fav[i].url, config.station.url)) return i;
  return -1;
}

/*  Номер станції шукаємо в плейлисті за адресою. Файл — 60 рядків на
    SPIFFS, прохід займає кілька мілісекунд.  */
bool YoExtras::favPlay(uint8_t i){
  if(i >= FAV_N || !fav[i].url[0]) return false;
  File f = SPIFFS.open(PLAYLIST_PATH, "r");
  if(!f) return false;
  uint16_t n = 0, found = 0;
  char line[320];
  while(f.available()){
    size_t l = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[l] = 0;
    n++;
    char* t1 = strchr(line, '\t'); if(!t1) continue;
    char* t2 = strchr(t1 + 1, '\t'); if(t2) *t2 = 0;
    if(!strcmp(t1 + 1, fav[i].url)){ found = n; break; }
  }
  f.close();
  if(!found) return false;               /* станцію з плейлиста прибрали */
  if(config.getMode() == PM_SDCARD){
    config.changeMode(PM_WEB);
    player.resetQueue();                 /* без «грати останню» від переходу на радіо */
  }
  player.sendCommand({PR_PLAY, (int)found});
  return true;
}

void YoExtras::changed(){ _dirty = true; _dirtyMs = millis(); }

void YoExtras::wifiPending(const char* ssid){
  Preferences p;
  if(!p.begin("yoext", false)) return;
  p.putString("wpend", ssid);
  p.end();
}

/*  Мережі є в пам'яті, а файл порожній — таке вже траплялось (обірвана
    заливка обрізає файл першим же байтом). Мовчки кладемо список назад:
    інакше після перезавантаження радіо лишилось би без жодної мережі.  */
void YoExtras::_wifiFileGuard(uint32_t now){
  static bool done = false;
  if(done || now < 20000) return;
  done = true;
  if(config.ssidsCount == 0) return;
  File f = SPIFFS.open(SSIDS_PATH, "r");
  size_t sz = f ? f.size() : 0;
  if(f) f.close();
  if(sz > 2) return;
  String out;
  for(uint8_t i = 0; i < config.ssidsCount; i++)
    out += String(config.ssids[i].ssid) + "\t" + String(config.ssids[i].password) + "\n";
  if(config.saveWifiList(out.c_str()))
    Serial.printf("##WIFI#\tсписок мереж (%u) відновлено з пам'яті\n", (unsigned)config.ssidsCount);
}

/*  Через кілька секунд після старту мережа вже або є, або плата пішла
    в точку доступу. Порівнюємо з тим, чого просили, і забуваємо запит.  */
void YoExtras::_wifiCheck(uint32_t now){
  if(!_wpend[0] || now < 4000) return;
  if(network.status != CONNECTED && network.status != SOFT_AP) return;
  bool ok = network.status == CONNECTED && WiFi.SSID() == String(_wpend);
  if(!ok){
    strlcpy(wifiFail, _wpend, sizeof(wifiFail));
    if(network.status == CONNECTED){
      snprintf(config.tmpBuf, sizeof(config.tmpBuf), "Wi-Fi: не вдалося підключитись до %s", _wpend);
      config.setTitle(config.tmpBuf);
    }
  }
  Serial.printf("##WIFI#\tзапит на %s: %s\n", _wpend, ok ? "підключено" : "не вдалося");
  _wpend[0] = 0;
  Preferences p;
  if(p.begin("yoext", false)){ p.remove("wpend"); p.end(); }
}

/*  Аудіовихід. Вбудований кодек ES8311 сидить на GPIO 4/5/7/8; зовнішній
    ЦАП I2S (PCM5102A, UDA1334A, MAX98357A) — на вільних виводах роз'єму:
    BCLK 14, LRC 21, DIN 2. Перемикаємо самі виводи I2S на ходу, а
    вбудований підсилювач вимикаємо, щоб динамік плати мовчав.  */
void YoExtras::applyDac(){
#if !EXT_DAC
  player.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_DIN, I2S_MCLK);
  player.setOutputPins(player.status() == PLAYING);
  return;
#endif
  if(s.dac){
    player.setPinout(DAC_BCLK, DAC_LRC, DAC_DOUT, I2S_PIN_NO_CHANGE, I2S_PIN_NO_CHANGE);
    /*  Відв'язуємо лінію даних вбудованого кодека: перепризначення виводів не
        знімає старий із матриці GPIO, тож ES8311 і далі отримував звук і
        вбудований динамік говорив разом із зовнішнім ЦАПом. Такт і слово
        (BCLK/LRC) лишаємо — на них тримається вбудований мікрофон.  */
    gpio_reset_pin((gpio_num_t)I2S_DOUT);
    pinMode(I2S_DOUT, OUTPUT); digitalWrite(I2S_DOUT, LOW);   /* нуль, а не підтяжка вгору */
  }else{
    player.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_DIN, I2S_MCLK);
    /*  Старі виводи зовнішнього ЦАП відв'язуємо від шини й КЛАДЕМО В НУЛЬ.
        Саме відв'язування лишає на них підтяжку вгору, і підключений модуль
        живиться через неї крізь захисні діоди своїх входів — світиться, хоч
        звук іде не на нього.  */
    static const uint8_t DP[3] = { DAC_BCLK, DAC_LRC, DAC_DOUT };
    for(uint8_t i = 0; i < 3; i++){
      gpio_reset_pin((gpio_num_t)DP[i]);
      pinMode(DP[i], OUTPUT);
      digitalWrite(DP[i], LOW);
    }
  }
  player.setOutputPins(player.status() == PLAYING);   /* вбудований підсилювач і MUTE зовнішнього */
  Serial.printf("##DAC#\tаудіовихід %u, MUTE підсилювача на " EXT_MUTE_NAME ": %u\n", s.dac, s.extMute);
}

/*  MUTE зовнішнього підсилювача (чи вхід XSMT ЦАП) на EXT_MUTE_PIN. «Звук» — лише коли звук
    справді йде на зовнішній ЦАП; інакше, і поки радіо мовчить, — «тихо». До запуску прошивки
    вивід ніхто не веде (він вхід UART), тому «тихо» на старті тримає резистор 10 кОм: на GND
    для режиму «тихо — низьким», на 3,3 В для «тихо — високим».  */
void YoExtras::extAmp(bool sound){
#if EXT_DAC
  if(!s.extMute || EXT_MUTE_PIN == 255) return;
  const bool on = sound && s.dac != 0;
  if(_extSet && on == _extOn) return;
  _extOn = on;
  if(!_extSet){ pinMode(EXT_MUTE_PIN, OUTPUT); _extSet = true; }
  const bool lvl = (s.extMute == 1) ? on : !on;          /* 1: тихо — низьким (звук = високий); 2: тихо — високим */
  digitalWrite(EXT_MUTE_PIN, lvl ? HIGH : LOW);
  atrace::ev(on ? "MUTE знято" : "MUTE");
  Serial.printf("##AMP#\tзовнішній підсилювач: %s (" EXT_MUTE_NAME " = %d) на %lu мс\n", on ? "звук" : "тихо", lvl ? 1 : 0, (unsigned long)millis());
#else
  (void)sound;
#endif
}

/*  Звук іде просто зараз. Знімаємо MUTE одразу — на першому ж записі в I2S: у DMA ще ~93 мс
    до того, як цей звук пролунає, підсилювач (чи плавне зняття XSMT) якраз устигає.  */
#define EXT_HOLD_MS 300                  /* звук не йде стільки — тихо (довше за DMA 93 мс, щоб не зрізати хвіст) */
void YoExtras::extAmpKick(bool fromPlayer){
#if EXT_DAC
  if(!s.extMute || s.dac == 0) return;
  if(fromPlayer){
    const int16_t v = player.volOverride >= 0 ? player.volOverride : (int16_t)config.store.volume;
    if(v <= 0) return;                   /* гучність 0 — звуку немає: хай глушиться */
  }
  _extAt = millis() | 1;
  if(_extHold){ if((int32_t)(millis() - _extHold) < 0) return; _extHold = 0; }   /* ЦАП ще ловить такт */
  if(!_extOn) extAmp(true);
#else
  (void)fromPlayer;
#endif
}

/*  Зміна частоти I2S (проповідь 48 кГц → станція 44,1 кГц). Власник 03.11.2026: «при переключенні на радіо
    після проповідей — короткі гучні артефакти перед початком нормального звуку». atrace показав: цифровий
    звук у цю мить — тиша (−99 дБ, затихання), а мікрофон чує −25 дБ (фон −55…−67) через 9 мс після зміни
    частоти; MUTE знімався вже через 6 мс. Зі станції на станцію (частота та сама) зняття MUTE не клацає.
    Тобто клацає ЦАП, поки ловить новий такт. Тепер MUTE — ще до зміни і 200 мс після: нарощування звуку
    (0,7 с) однаково починається з нуля, а ЦАП устигає.  */
#define EXT_RATE_HOLD_MS 200
void YoExtras::rateChange(){
#if EXT_DAC
  if(!s.extMute || s.dac == 0) return;
  _extHold = (millis() + EXT_RATE_HOLD_MS) | 1;
  if(_extOn) extAmp(false);
#endif
}

void YoExtras::_extAmpLoop(uint32_t now){
  if(_extOn && (int32_t)(now - _extAt) > EXT_HOLD_MS) extAmp(false);
}

/*  Вивід плеєра (Audio.cpp, yoOutFlush): кожна пачка відліків у I2S — «звук іде». Лише поки плеєр
    справді грає: після зупинки ще встигала проскочити остання пачка, і MUTE знімався на 0,3 с
    (на платі: тихо → через 6 мс звук → тихо).  */
void yoAudioOut(){ if(extras.s.extMute && player.status() == PLAYING) extras.extAmpKick(true); }
void yoRateChange(){ extras.rateChange(); }

void YoExtras::extMuteApply(){
#if EXT_DAC
  if(EXT_MUTE_PIN == 255) return;
  gpio_hold_dis((gpio_num_t)EXT_MUTE_PIN);
  if(!s.extMute){
    pinMode(EXT_MUTE_PIN, INPUT);                        /* вимкнено — вивід не ведемо зовсім */
    _extSet = false; _extOn = false;
    Serial.println("##AMP#\tMUTE зовнішнього підсилювача вимкнено — " EXT_MUTE_NAME " відпущено");
    return;
  }
  _extSet = false;                                       /* режим змінився — виставити рівень заново */
  extAmp(player.status() == PLAYING && millis() - _extAt < EXT_HOLD_MS);
#endif
}

/*  Перед будь-яким перезапуском (меню, оновлення, сторож, сторінка) — записати відкладені
    налаштування: під час звуку їх тримаємо до тиші (див. Config::eepromLoop).  */
static void yoShutdownSave(){ extras.extAmp(false); config.commitNow(); extras.saveNow(); }   /* зовнішній підсилювач — тихо до перезапуску */

void YoExtras::begin(){
  esp_register_shutdown_handler(yoShutdownSave);
  {
    Preferences p;
    if(p.begin("yoext", true)){
      if(p.isKey("wpend")) p.getString("wpend", _wpend, sizeof(_wpend));
      p.end();
    }
  }
  _load();
  applyDac();          /* виставляємо вибраний вихід ДО першого звуку (привітання заставки) */
  analogSetPinAttenuation(EXT_BAT_PIN, ADC_11db);
  _led(0, 0, 0);
}

void YoExtras::loop(){
  uint32_t now = millis();
  _extAmpLoop(now);
  _sleepLoop(now);
  _alarmLoop(now);
  _alarmWakeLoop(now);
  _lowBatLoop(now);
  _screenLoop(now);
  _batLoop(now);
  _ledLoop(now);
  recorder.loop();
  logos.loop();                      /* логотипи всіх станцій — раз і з кнопки */
  yoWebApiLoop();                    /* команди з веб-сторінки */
  _wifiCheck(now);
  _wifiFileGuard(now);
  /*  Під час звуку — лише після 5 хв без змін (запис NVS зупиняє ядра, див. Config::eepromLoop);
      решту запише тиша між станціями, сон чи перезапуск.  */
  if(_dirty && now - _dirtyMs > 3000 && (!(player.isRunning() || player.extOn) || now - _dirtyMs > 300000UL)){ _dirty = false; _save(); }
  if(_pwrMode && (int32_t)(now - _pwrAt) >= 0){
    uint8_t m = _pwrMode; _pwrMode = 0;
    if(_dirty){ _dirty = false; _save(); }
    if(m == 1){
      Serial.println("##POWER#\tперезавантаження з меню");
      recorder.stop();
      delay(150);
      ESP.restart();
    } else _powerOff();
  }
}

/*  ---------- живлення ----------
    «Вимкнути» — глибокий сон ESP32-S3: Wi-Fi, процесор і пам'ять знеструмлені,
    лишається RTC, що чекає дотику. Решту плати гасимо самі: підсилювач
    (вивід MUTE), кодек, підсвітку, світлодіод, екран (його присипляє задача
    дисплея). Сенсор FT6336 лишаємо в ощадливому режимі спостереження — він і
    будить: його INT на GPIO17 (вивід RTC) іде в нуль, поки палець на екрані.
    Виводи, що мають тримати рівень у сні, фіксуємо (gpio_hold), бо інакше
    вони «пливуть» і, наприклад, підсилювач може ввімкнутись.  */
#define FT_ADDR 0x38
static void ftWrite(uint8_t reg, uint8_t val){
  Wire.beginTransmission(FT_ADDR); Wire.write(reg); Wire.write(val); Wire.endTransmission();
}

void YoExtras::_powerOff(){
  Serial.println("##POWER#\tвимикаюсь: сон до дотику");
  bool was = player.status() == PLAYING && !player.remoteStationName;
  recorder.stop();
  /*  Звук — затихає тут же, у головному циклі. Раніше зупинка йшла чергою, а
      цикл чекав у delay(): плеєр у цей час не крутився, і за секунду радіо
      засинало з обірваним звуком.  */
  if(player.status() == PLAYING) player.fadeStop();
  /*  грало радіо — після ввімкнення хай грає далі (якщо «грати після ввімкнення» не вимкнено)  */
  if(was && config.store.smartstart != 2) config.saveValue(&config.store.smartstart, (uint8_t)1);
  config.commitNow();                  /* відкладені налаштування — до сну (у звуку їх не писали) */
  saveNow();
  if(sdman.ready) sdman.stop();
  _led(0, 0, 0);
  pwmSet(0);
  /*  екран: з меню його вже приспала задача дисплея; зі сторінки — спершу
      зупиняємо малювання, тоді присипляємо  */
  display.lock();
  delay(80);
  display.deepsleep();
  delay(50);

#if defined(I2S_ES8311) && I2S_ES8311
  es8311_suspend();
#endif
  /*  сенсор: INT тримається низько весь час дотику (режим опитування),
      сам переходить у ощадливий режим спостереження через 1 с без дотиків  */
#if TS_MODEL==TS_MODEL_FT6336
  ftWrite(0xA4, 0x00);
  ftWrite(0x86, 0x01);
  ftWrite(0x87, 0x01);
#endif

  pinMode(MUTE_PIN, OUTPUT);       digitalWrite(MUTE_PIN, MUTE_VAL);  gpio_hold_en((gpio_num_t)MUTE_PIN);
  pinMode(BRIGHTNESS_PIN, OUTPUT); digitalWrite(BRIGHTNESS_PIN, LOW); gpio_hold_en((gpio_num_t)BRIGHTNESS_PIN);
  pinMode(TS_RST, OUTPUT);         digitalWrite(TS_RST, HIGH);        gpio_hold_en((gpio_num_t)TS_RST);
  pinMode(EXT_LED_PIN, OUTPUT);    digitalWrite(EXT_LED_PIN, LOW);    gpio_hold_en((gpio_num_t)EXT_LED_PIN);
  /*  Виводи звуку теж кладемо в нуль і фіксуємо. Уві сні вони інакше «пливуть»
      або лишаються з підтяжкою, і зовнішній модуль живиться через свої вхідні
      діоди — світиться при вимкненому радіо. Лінію мікрофона (I2S_DIN) не
      чіпаємо: це вхід, її веде кодек.  */
  {
    static const uint8_t SP[7] = { DAC_BCLK, DAC_LRC, DAC_DOUT, I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_MCLK };
    for(uint8_t i = 0; i < 7; i++){
      if(SP[i] == 255) continue;
      gpio_reset_pin((gpio_num_t)SP[i]);
      pinMode(SP[i], OUTPUT);
      digitalWrite(SP[i], LOW);
      gpio_hold_en((gpio_num_t)SP[i]);
    }
  }
  /*  MUTE зовнішнього підсилювача — у «тихо» й зафіксувати на весь сон (і рівень залежить від
      режиму: при «тихо — високим» нуль якраз увімкнув би підсилювач на всю ніч).  */
  if(s.extMute && EXT_MUTE_PIN != 255){
    extAmp(false);
    gpio_hold_en((gpio_num_t)EXT_MUTE_PIN);
  }
  gpio_deep_sleep_hold_en();

#if TS_MODEL==TS_MODEL_ST77922
  _sleepUntilTouch();                            /* ES3C35P: див. нижче; звідти не повертається */
#endif
  /*  палець ще на кнопці «вимкнути» — дочекатись, поки відпустять,
      інакше радіо прокинулось би тієї ж миті  */
  pinMode(TS_INT, INPUT_PULLUP);
  for(int i = 0; i < 300 && digitalRead(TS_INT) == LOW; i++) delay(10);
  delay(200);

  rtc_gpio_init((gpio_num_t)TS_INT);
  rtc_gpio_set_direction((gpio_num_t)TS_INT, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)TS_INT);
  rtc_gpio_pulldown_dis((gpio_num_t)TS_INT);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)TS_INT, 0);
  _armAlarmWake();
  Serial.flush();
  esp_deep_sleep_start();
}

/*  ---------- будильник, коли радіо «вимкнене» ----------
    Годинник у сні йде від внутрішнього RC-генератора: за ніч він помиляється
    на відсотки (хвилини). Тож прокидаємось із запасом (1,5 хв + 3 % від сну),
    тихо стартуємо, беремо точний час із мережі й вирішуємо: ще далеко — знову
    спати (новий відрізок коротший, помилка менша), близько — чекати в темряві
    й дзвонити, як завжди.  */
RTC_DATA_ATTR static uint8_t s_alarmWakeArmed = 0;
RTC_DATA_ATTR static uint8_t s_softTouchWake = 0;  /* ES3C35P: прокинулись від дотику в легкому сні й перезапустились */
RTC_DATA_ATTR static int64_t s_alarmTestAt = 0;   /* перевірка (команда alarmtest): «будильник» на цю мить, переживає сон */
static bool s_wokeTouch = false;
static bool s_wokeAlarm = false;
static bool s_alarmQuiet = false;
bool YoExtras::wokeByTouch(){ return s_wokeTouch; }
bool YoExtras::wokeForAlarm(){ return s_wokeAlarm; }

/*  Автостарт (власник 03.11.2026: «при включенні радіо має бути автозапуск останньої станції»).
    Увімкнули живленням, кнопкою скидання чи збудили дотиком — грає остання станція завжди, навіть
    коли перед вимкненням стояла (зупинили, обірвався потік: тоді smartstart = 0, і радіо мовчало —
    так було 03.11 о 19:06). Після перезавантаження програмою (оновлення), сторожем чи збоєм —
    як і було: грає, лише якщо грало. «Автостарт» вимкнено (2) чи будить будильник — не грає.  */
bool YoExtras::bootAutoPlay(){
  if(config.store.smartstart == 2 || s_wokeAlarm) return false;
  const esp_reset_reason_t r = esp_reset_reason();
  if(r == ESP_RST_POWERON || r == ESP_RST_EXT || r == ESP_RST_DEEPSLEEP) return true;
  return config.store.smartstart == 1;
}
bool YoExtras::alarmQuiet(){ return s_alarmQuiet; }

int64_t YoExtras::_alarmSleepSecs(){
  s_alarmWakeArmed = 0;
  const int64_t secs = _alarmSecs();
  if(secs <= 0) return -1;
  int64_t margin = 90 + secs * 3 / 100;
  int64_t sl = secs - margin;
  if(sl < 5) sl = 5;
  s_alarmWakeArmed = 1;
  Serial.printf("##ALARM#\tбудильник через %lld с — прокинусь через %lld с\n", (long long)secs, (long long)sl);
  return sl;
}

void YoExtras::_armAlarmWake(){
  const int64_t sl = _alarmSleepSecs();
  if(sl > 0) esp_sleep_enable_timer_wakeup((uint64_t)sl * 1000000ULL);
}

int64_t YoExtras::_alarmSecs() const {
  if(s_alarmTestAt){ const int64_t d = s_alarmTestAt - (int64_t)time(nullptr); return d > 0 ? d : -1; }
  if(!s.alarmOn) return -1;
  time_t now = time(nullptr);
  if(now < 1600000000) return -1;                 /* годинник ще не звірений */
  struct tm t; localtime_r(&now, &t);
  const int32_t cur = t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
  const int32_t at = s.alarmH * 3600 + s.alarmM * 60;
  for(int d = 0; d < 8; d++){
    const int w = (t.tm_wday + d) % 7;
    if(s.alarmDays == 1 && (w == 0 || w == 6)) continue;
    const int64_t x = (int64_t)d * 86400 + at - cur;
    if(x > 0) return x;
  }
  return -1;
}

int32_t YoExtras::_alarmPassedSecs() const {
  if(s_alarmTestAt){ const int64_t d = (int64_t)time(nullptr) - s_alarmTestAt; return d >= 0 ? (int32_t)d : -1; }
  if(!s.alarmOn) return -1;
  time_t now = time(nullptr);
  if(now < 1600000000) return -1;
  struct tm t; localtime_r(&now, &t);
  if(s.alarmDays == 1 && (t.tm_wday == 0 || t.tm_wday == 6)) return -1;
  const int32_t d = t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec - (s.alarmH * 3600 + s.alarmM * 60);
  return d >= 0 ? d : -1;
}

void YoExtras::_alarmWakeLoop(uint32_t now){
  if(!s_alarmQuiet || _pwrMode) return;
  static uint32_t t = 0;
  if(now - t < 1000) return;
  t = now;
  /*  станцію ввімкнули звідкись (сторінка, програма) — радіо вже просто працює  */
  if(player.isRunning()){ Serial.println("##ALARM#\tстанцію ввімкнули до будильника — радіо увімкнене"); s_alarmQuiet = false; return; }
  /*  Точний час: мережа піднялась — ще 8 с на звірку годинника (SNTP), або
      синхронізація вже відзвітувала; без мережі — не довше хвилини від старту.
      Раніше чекали до 2 хвилин і з коротким запасом пропускали хвилину будильника.  */
  static uint32_t netT = 0;
  if(!netT && WiFi.status() == WL_CONNECTED) netT = now;
  static bool synced = false;
  if(!synced && sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) synced = true;
  if(!synced && !(netT && now - netT > 8000) && now < 60000UL) return;
  const int32_t passed = _alarmPassedSecs();
  if(passed >= 0 && passed < 600 && (s_alarmTestAt || _lastAlarmKey != s.alarmH * 60 + s.alarmM)){
    /*  прокинулись пізніше, ніж треба (годинник у сні відстав) — дзвонимо одразу  */
    if(passed < 5) Serial.println("##ALARM#\tчас будильника — дзвоню");
    else Serial.printf("##ALARM#\tпрокинувся на %ld с пізніше за будильник — дзвоню\n", (long)passed);
    if(!s_alarmTestAt) _lastAlarmKey = s.alarmH * 60 + s.alarmM;
    s_alarmTestAt = 0;
    s_alarmQuiet = false;
    _alarmStart();
    return;
  }
  const int64_t secs = _alarmSecs();
  if(secs < 0 || secs > 300){
    Serial.printf("##ALARM#\tдо будильника %lld с — знову сплю\n", (long long)secs);
    _powerOff();                                   /* сам поставить наступне пробудження (або сон до дотику) */
    return;
  }
  /*  лишилось до 5 хвилин — чекаємо в темряві, _alarmLoop подзвонить  */
}

/*  Пам'ять для TLS — у PSRAM. У цій збірці ядра mbedTLS бере лише
    внутрішню RAM (CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC), а кожне з'єднання
    https хоче ~40 КБ одним шматком. Поки грає станція по https, а пам'ять
    за години роботи дрібнішає, список проповідей переставав вантажитись
    («сайт недоступний») аж до перезавантаження. PSRAM на платі 8 МБ.  */
static void* tlsCalloc(size_t n, size_t sz){
  void* p = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_calloc(n, sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
static void tlsFree(void* p){ heap_caps_free(p); }

void YoExtras::earlyBoot(){
  mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree);
  s_wokeTouch = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0 || s_softTouchWake;
  s_softTouchWake = 0;
  s_wokeAlarm = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER && s_alarmWakeArmed;
  s_alarmWakeArmed = 0;
  s_alarmQuiet = s_wokeAlarm;
  /*  фіксацію, поставлену перед сном, знімаємо — інакше виводи так і лишились би
      «замороженими»: підсвітка не світила б, підсилювач мовчав  */
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)MUTE_PIN);
  gpio_hold_dis((gpio_num_t)BRIGHTNESS_PIN);
  gpio_hold_dis((gpio_num_t)TS_RST);
  gpio_hold_dis((gpio_num_t)EXT_LED_PIN);
  /*  Виводи звуку теж були зафіксовані в нулі (щоб уві сні не живили
      зовнішній модуль) — відпускаємо, інакше радіо прокинулось би німим.  */
  {
    static const uint8_t SP[8] = { DAC_BCLK, DAC_LRC, DAC_DOUT, I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_MCLK, EXT_MUTE_PIN };
    for(uint8_t i = 0; i < 8; i++) if(SP[i] != 255) gpio_hold_dis((gpio_num_t)SP[i]);
  }
  if(s_wokeTouch) rtc_gpio_deinit((gpio_num_t)TS_INT);
}

static bool timeValid(){ return network.timeinfo.tm_year > 100; }

/*  ---------- таймер сну ---------- */

void YoExtras::setSleep(uint16_t minutes){
  _sleepSet = minutes;
  if(_sleepFading){                       /* повертаємо гучність, що була */
    _sleepFading = false;
    player.volOverride = -1;
    player.applyVol(config.store.volume);
  }
  _sleepEnd = minutes ? millis() + minutes * 60000UL : 0;
  if(_sleepEnd == 0 && minutes) _sleepEnd = 1;
}

/*  Чому радіо перезавантажилось востаннє — щоб «зависло й перезавантажилось»
    мало ім'я: сторожовий таймер, падіння програми чи просідання живлення.  */
const char* YoExtras::resetReason(){
  switch(esp_reset_reason()){
    case ESP_RST_POWERON:   return "увімкнення живлення";
    case ESP_RST_EXT:       return "зовнішній скид";
    case ESP_RST_SW:        return "перезавантаження програмою";
    case ESP_RST_PANIC:     return "ПАДІННЯ ПРОГРАМИ (panic)";
    case ESP_RST_INT_WDT:   return "СТОРОЖ ПЕРЕРИВАНЬ (зависання)";
    case ESP_RST_TASK_WDT:  return "СТОРОЖ ЗАДАЧ (зависання)";
    case ESP_RST_WDT:       return "СТОРОЖОВИЙ ТАЙМЕР";
    case ESP_RST_DEEPSLEEP: return "вихід із глибокого сну";
    case ESP_RST_BROWNOUT:  return "ПРОСІДАННЯ ЖИВЛЕННЯ (brownout)";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB";
    case ESP_RST_JTAG:      return "JTAG";
    default:                return "невідомо";
  }
}

/*  У кімнаті тихо (мікрофон) — починаємо затихання зараз, а не наприкінці.  */
bool YoExtras::sleepSoon(){
  if(!_sleepEnd) return false;
  int32_t left = (int32_t)(_sleepEnd - millis());
  if(left <= (int32_t)SLEEP_FADE_MS) return false;
  _sleepEnd = millis() + SLEEP_FADE_MS;
  return true;
}

void YoExtras::sleepTestSec(uint32_t sec){
  setSleep(1);
  _sleepEnd = millis() + sec * 1000UL;
}

uint32_t YoExtras::sleepLeftSec() const {
  if(!_sleepEnd) return 0;
  int32_t l = (int32_t)(_sleepEnd - millis());
  return l > 0 ? (uint32_t)(l + 999) / 1000 : 0;
}

uint16_t YoExtras::sleepLeft() const {
  if(!_sleepEnd) return 0;
  int32_t l = (int32_t)(_sleepEnd - millis());
  return l > 0 ? (uint16_t)((l + 59999) / 60000) : 0;
}

void YoExtras::_sleepLoop(uint32_t now){
  if(!_sleepEnd) return;
  int32_t left = (int32_t)(_sleepEnd - now);
  if(left <= 0){
    /*  Кінець: зупиняємо й гасимо екран. Гучність у пам'яті не чіпали,
        тож наступний запуск заграє так, як було до сну.  */
    _sleepEnd = 0; _sleepSet = 0;
    _sleepFading = false;
    player.volOverride = -1;
    if(player.status() == PLAYING) player.sendCommand({PR_STOP, 0});
    sfx.play(SFX_TIMER);
    _dark = true;
    return;
  }
  if(left <= (int32_t)SLEEP_FADE_MS && player.status() == PLAYING){
    uint8_t v = (uint32_t)config.store.volume * (uint32_t)left / SLEEP_FADE_MS;
    if(!_sleepFading || player.volOverride != v){
      _sleepFading = true;
      player.volOverride = v;
      player.applyVol(v);
    }
  }
}

/*  ---------- будильник ---------- */

static uint8_t rampState = 0;            /* 0 — нема, 1 — чекаємо звуку, 2 — наростає, 3 — ще дзвенить дзвіночок */
static uint32_t rampWait0 = 0;
static uint32_t rampPlayAt = 0;          /* коли вмикати станцію (після дзвіночка) */
static uint8_t  rampBase = 0;            /* гучність у налаштуваннях на момент старту */

void YoExtras::alarmNow(){ _alarmStart(); }

void YoExtras::alarmTestOff(uint16_t sec){
  s_alarmTestAt = (int64_t)time(nullptr) + sec;
  Serial.printf("##ALARM#\tперевірка: вимикаюсь, «будильник» через %u с (на %lld)\n", (unsigned)sec, (long long)s_alarmTestAt);
  requestPower(2);
}

void YoExtras::_alarmStart(){
  uint32_t now = millis();
  s_alarmQuiet = false;
  s_alarmTestAt = 0;
  _dark = false;
  _wakeUntil = now + 60000UL;            /* хвилину екран світить і вночі */
  if(_sleepEnd) setSleep(0);
  if(player.status() == PLAYING) return;   /* уже грає — не заважаємо */
  if(config.getMode() == PM_SDCARD) config.changeMode(PM_WEB);
  rampBase = config.store.volume;
  _rampTo = rampBase < ALARM_MIN_VOL ? ALARM_MIN_VOL : rampBase;
  player.volOverride = 0;
  player.applyVol(0);
  /*  Спершу дзвіночок цілком, потім станція. Разом вони заїкались: з'єднання
      (TLS ~0,4 с) ішло пріоритетніше за звук, а далі дзвіночок домішувався в
      потік, що саме набирав буфер.  */
  if(sfx.willPlay(SFX_ALARM)){
    sfx.play(SFX_ALARM);
    rampPlayAt = millis() + sfx.clipMs(SFX_ALARM) + 350 + 400;   /* + пробудження підсилювача + хвіст */
    rampState = 3;
  }else{
    player.sendCommand({PR_PLAY, config.lastStation()});
    rampState = 1;
  }
  rampWait0 = now;
  _rampT0 = now;                         /* alarmRinging() */
}

int32_t YoExtras::alarmInMin() const {
  if(!s.alarmOn || !timeValid()) return -1;
  int cur = network.timeinfo.tm_hour * 60 + network.timeinfo.tm_min;
  int wd  = network.timeinfo.tm_wday;
  int at  = s.alarmH * 60 + s.alarmM;
  for(int d = 0; d < 8; d++){
    int w = (wd + d) % 7;
    if(s.alarmDays == 1 && (w == 0 || w == 6)) continue;
    int m = d * 1440 + at - cur;
    if(m > 0) return m;
  }
  return -1;
}

void YoExtras::_alarmLoop(uint32_t now){
  static uint32_t tick = 0;
  if(now - tick < 100) return;
  tick = now;

  if(rampState){
    /*  Гучність рухнули пальцем — людина прокинулась, далі вона сама.  */
    if(config.store.volume != rampBase){
      rampState = 0; _rampT0 = 0; player.volOverride = -1;
    }else if(rampState == 3){
      if((int32_t)(now - rampPlayAt) >= 0){ player.sendCommand({PR_PLAY, config.lastStation()}); rampState = 1; rampWait0 = now; }
    }else if(rampState == 1){
      if(player.status() == PLAYING){ rampState = 2; _rampT0 = now; }
      else if(now - rampWait0 > ALARM_WAIT_MS){ rampState = 0; _rampT0 = 0; player.volOverride = -1; }
    }else{
      uint32_t t = now - _rampT0;
      if(player.status() != PLAYING){ rampState = 0; _rampT0 = 0; player.volOverride = -1; }
      else if(t >= ALARM_RAMP_MS){
        rampState = 0; _rampT0 = 0; player.volOverride = -1;
        if(_rampTo != config.store.volume) player.setVol(_rampTo);
        else player.applyVol(_rampTo);
      }else{
        uint8_t v = (uint32_t)_rampTo * t / ALARM_RAMP_MS;
        if(player.volOverride != v){ player.volOverride = v; player.applyVol(v); }
      }
    }
  }

  if(!s.alarmOn || !timeValid()) return;
  int key = network.timeinfo.tm_hour * 60 + network.timeinfo.tm_min;
  if(network.timeinfo.tm_hour != s.alarmH || network.timeinfo.tm_min != s.alarmM){
    _lastAlarmKey = -1;
    return;
  }
  if(_lastAlarmKey == key) return;
  _lastAlarmKey = key;
  int wd = network.timeinfo.tm_wday;
  if(s.alarmDays == 1 && (wd == 0 || wd == 6)) return;
  _alarmStart();
}

/*  ---------- батарея сідає ----------
    Поки заряд нижче 10 % і зарядника немає: раз на хвилину сигнал (звучить
    навіть із вимкненими звуками подій) і картка на екрані; пригашений екран
    на кілька секунд засвічується. Перше попередження — через 5 с після того,
    як рівень упав (щоб не смикатись від одного виміру).  */
void YoExtras::_lowBatLoop(uint32_t now){
  if(!lowBattery() || s_alarmQuiet){ _lowSince = 0; _lowNext = 0; return; }
  if(!_lowSince){ _lowSince = now; _lowNext = now + 5000; }
  if((int32_t)(now - _lowNext) < 0) return;
  _lowNext = now + 60000UL;
  _lowBeat++;
  _attnUntil = now + 8000;
  _wakeUntil = now + 8000;                  /* уночі — теж на денну яскравість */
  sfx.test(SFX_LOWBAT);
  Serial.printf("##POWER#\tбатарея сідає: %d%%, %u мВ\n", batPct(), (unsigned)_batMv);
#ifdef USE_YOMENU
  if(m2::M.active()){ char b[96]; snprintf(b, sizeof(b), "батарея %d%% — під'єднайте зарядку", batPct()); m2::M.toast(b); }
#endif
}

/*  ---------- екран ---------- */

uint16_t YoExtras::pwmTarget(){
#if BRIGHTNESS_PIN!=255
  if(!config.store.dspon || _blank || s_alarmQuiet) return 0;
  const bool attn = (int32_t)(_attnUntil - millis()) > 0;   /* попередження про батарею — екран світить, хоч би що */
  if(_dark && !attn) return 0;
#ifdef USE_YOMENU
  if(_presDark && !yomenu.active() && !attn) return 0;        /* мікрофон: у кімнаті давно нікого не чути */
#else
  if(_presDark && !attn) return 0;
#endif
  uint16_t day = map(config.store.brightness, 0, 100, 0, 255);
  bool awake = (int32_t)(_wakeUntil - millis()) > 0;
#ifdef USE_YOMENU
  if(yomenu.active()) awake = true;      /* у налаштуваннях людина вже тут */
#endif
  uint16_t t = day;
  if(_night && !awake){
    uint16_t n = map(s.nightLevel, 0, 100, 0, 255);
    t = n < day ? n : day;
  }
  /*  Економія батареї: без зарядника й без дотиків N секунд — плавно до
      мінімуму. Екран при цьому видно, лише дуже тьмяно.  */
  static const uint8_t SAVE_S[5] = { 0, 10, 15, 30, 60 };
  _saver = s.batSave && !s.noBat && _batMv >= 2800 && !onPower() &&
           (millis() - _lastTouch) > (uint32_t)SAVE_S[s.batSave] * 1000UL;
  if(_saver && t > 6 && !attn) t = 6;
  return t;
#else
  return 255;
#endif
}

void YoExtras::pwmSet(uint16_t v){
#if BRIGHTNESS_PIN!=255
  analogWrite(BRIGHTNESS_PIN, v);
#endif
  if(millis() < 12000 && v != _pwmCur) Serial.printf("##DSP#\tпідсвітка %u на %u мс\n", (unsigned)v, (unsigned)millis());   /* старт: коли засвічується екран */
  _pwmCur = v;
}

uint16_t YoExtras::pwmNow(){ uint16_t v = pwmTarget(); pwmSet(v); return v; }

/*  Дотик у темряві лише будить: інакше, намацуючи погаслий екран, людина
    навмання натискала б кнопки, яких не бачить.  */
bool YoExtras::touchWake(){
  /*  Пригашений для економії екран теж спершу лише будимо: людина ще не
      бачить, куди тисне.  */
  bool wasDark = _dark || _saver || _presDark || s_alarmQuiet || (_pwmCur == 0 && config.store.dspon && !_blank);
  s_alarmQuiet = false;                      /* прокинулись до будильника й торкнулись — радіо просто ввімкнене */
  _dark = false;
  _presDark = false;
  _saver = false;
  _lastTouch = millis();
  _wakeUntil = millis() + WAKE_MS;
  return wasDark;
}

void YoExtras::_screenLoop(uint32_t now){
  static uint32_t nightTick = 0;
  if(now - nightTick >= 1000 || nightTick == 0){
    nightTick = now;
    if(_forceNight >= 0) _night = _forceNight;
    else if(!s.nightOn || !timeValid()) _night = false;
    else{
      int cur = network.timeinfo.tm_hour * 60 + network.timeinfo.tm_min;
      int from = s.nightFrom * 30, to = s.nightTo * 30;
      if(from == to) _night = false;
      else if(from < to) _night = (cur >= from && cur < to);
      else _night = (cur >= from || cur < to);
    }
  }
  if(now - _pwmTick < 20) return;
  _pwmTick = now;
  /*  Під час плавних переходів підсвіткою керують вони.  */
  if(display.fading()) return;
#ifdef USE_YOMENU
  if(yomenu.fading()) return;
#endif
  uint16_t t = pwmTarget();
  if(_pwmCur == 0xFFFF){ pwmSet(t); return; }
  if(_pwmCur == t) return;
  int d = (int)t - (int)_pwmCur;
  /*  Гасне повільно (секунди дві-три), засвічується швидко.  */
  int st = d < 0 ? abs(d) / 20 : abs(d) / 6;
  if(d < 0){ if(st < 1) st = 1; } else if(st < 3) st = 3;
  if(abs(d) <= st) pwmSet(t);
  else pwmSet(_pwmCur + (d > 0 ? st : -st));
}

/*  ---------- батарея ---------- */

void YoExtras::batSim(int8_t pct, int8_t chg){ _simPct = pct; _simChg = chg; }

uint16_t YoExtras::batRaw(){ return analogReadMilliVolts(EXT_BAT_PIN) * 2; }

/*  Розрядна крива літій-іонної банки під малим навантаженням.  */
static int8_t liionPct(uint16_t mv){
  static const uint16_t V[] = { 3300, 3500, 3600, 3700, 3750, 3800, 3900, 4000, 4100, 4200 };
  static const uint8_t  P[] = {    0,    4,   10,   24,   35,   46,   62,   76,   88,  100 };
  if(mv <= V[0]) return 0;
  for(uint8_t i = 1; i < sizeof(V)/sizeof(V[0]); i++)
    if(mv <= V[i]) return P[i-1] + (int)(P[i] - P[i-1]) * (mv - V[i-1]) / (V[i] - V[i-1]);
  return 100;
}

void YoExtras::_batLoop(uint32_t now){
  if(now - _batTick < 250) return;
  _batTick = now;
  _batAcc += analogReadMilliVolts(EXT_BAT_PIN);
  if(++_batN < 16) return;                 /* 16 вимірів = 4 секунди */
  uint16_t mv = (_batAcc / _batN) * 2;
  _batAcc = 0; _batN = 0;
  _batMv = _batMv ? (_batMv * 3 + mv) / 4 : mv;
  /*  Заряджання. Вивід CHRG зарядника TP4054 на цій платі нікуди не
      заведено, тож судимо з напруги: кабель підключили — вона за секунди
      стрибає вгору на 50..150 мВ, відключили — так само вниз. Повільний
      хід за п'ять хвилин підтверджує напрям, якщо стрибок проґавили.  */
  if(_batPrev){
    int d = (int)mv - (int)_batPrev;
    if(d >= 35) _charging = true;
    else if(d <= -35) _charging = false;
  }else if(mv >= 4180) _charging = true;   /* на старті: таке буває лише від зарядника */
  _batPrev = mv;
  if(!_batMinT || now - _batMinT >= 60000){
    _batMinT = now;
    if(_batMinN == 6){ memmove(_batMin, _batMin + 1, 5 * sizeof(uint16_t)); _batMinN = 5; }
    _batMin[_batMinN++] = mv;
    if(_batMinN == 6){
      int tr = (int)_batMin[5] - (int)_batMin[0];
      if(tr >= 20) _charging = true;
      else if(tr <= -20) _charging = false;
    }
  }
  /*  Живлення від USB. Вивід VBUS на цій платі до ESP32 не заведено
      (див. схему: VBUS іде лише на TP4054 і ключ Q3), тож: комп'ютер
      видно напевно — по службових кадрах USB (HWCDC::isPlugged), а
      простий зарядний блок — лише за стрибком напруги вище.  */
  bool host = HWCDC::isPlugged();
  _power = host || _charging;
  if(!_power) _full = false;
  else if(_batMv >= 4180) _full = true;          /* дійшли до 4.2 В — заряджено */
  else if(_batMv < 4120) _full = false;
  _usb = _power;
  _batPct = liionPct(_batMv);
}

/*  ---------- світлодіод ---------- */

void YoExtras::_led(uint8_t r, uint8_t g, uint8_t b){
  if(r == _ledR && g == _ledG && b == _ledB) return;
  _ledR = r; _ledG = g; _ledB = b;
  rgbLedWrite(EXT_LED_PIN, r, g, b);
}

void YoExtras::ledTest(uint8_t r, uint8_t g, uint8_t b, uint16_t ms){
  _ledTestUntil = millis() + ms;
  _led(r, g, b);
}

static void hsv(uint16_t h, uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b){
  h %= 360;
  uint8_t sct = h / 60, f = (h % 60) * 255 / 60;
  uint8_t q = (uint16_t)v * (255 - f) / 255, t = (uint16_t)v * f / 255;
  switch(sct){
    case 0: r=v; g=t; b=0; break;
    case 1: r=q; g=v; b=0; break;
    case 2: r=0; g=v; b=t; break;
    case 3: r=0; g=q; b=v; break;
    case 4: r=t; g=0; b=v; break;
    default: r=v; g=0; b=q; break;
  }
}

void YoExtras::_ledLoop(uint32_t now){
  if(now - _ledTick < 30) return;
  _ledTick = now;
  if(_ledTestUntil){
    if((int32_t)(now - _ledTestUntil) < 0) return;
    _ledTestUntil = 0;
  }
  bool awake = (int32_t)(_wakeUntil - now) > 0;
  if(s.ledMode == LED_OFF || _dark || (_night && !awake)){ _led(0, 0, 0); return; }

  bool net = (network.status == CONNECTED || network.status == SDREADY);
  bool playing = player.status() == PLAYING;
  /*  повільне «дихання» 0..1 з періодом близько двох секунд  */
  float br = 0.5f - 0.5f * cosf((now % 2000) * 6.2832f / 2000.0f);

  if(!net){ _led((uint8_t)(4 + LED_MAX * br), 0, 0); return; }
  if(rampState){ uint8_t w = 3 + (LED_MAX - 3) * br; _led(w, w * 3 / 4, w / 3); return; }
  if(recorder.active()){ _led((now / 500) % 2 ? LED_MAX : 2, 0, 0); return; }   /* запис — червоне блимання */
  if(lowBattery()){ _led((now % 2000) < 150 ? LED_MAX : 0, 0, 0); return; }     /* сідає батарея — короткий червоний спалах */

  if(s.ledMode == LED_MUSIC && playing){
    uint16_t v = player.get_VUlevel(255);
    int l = 255 - (int)(v >> 8), r = 255 - (int)(v & 0xFF);
    float lvl = (l > r ? l : r) / 255.0f;
    if(lvl > _ledEnv) _ledEnv = lvl; else _ledEnv *= 0.86f;
    uint8_t R, G, B;
    hsv((now / 60) % 360, (uint8_t)(1 + (LED_MAX - 1) * _ledEnv), R, G, B);
    _led(R, G, B);
    return;
  }
  if(!playing){ _led(6, 3, 0); return; }                       /* стоїть — бурштин */
  if(_sleepEnd){ _led(10, 0, 18); return; }                    /* чекає сну — фіолет */
  if(config.getMode() == PM_SDCARD){ _led(0, 6, 22); return; } /* картка — синій */
  _led(0, 18, 3);                                              /* радіо — зелений */
}

#if TS_MODEL==TS_MODEL_ST77922
/*  ---------- ES3C35P: «вимкнено» = легкий сон до дотику ----------
    Переривання сенсора ST77922 заведене на GPIO47, а будити з глибокого сну
    вміють лише виводи RTC (0..21). Тому радіо засинає легким сном. Кнопки
    радіо не читає зовсім (рішення власника 25.10.2026: лише сенсор).
    Сенсор живе в контролері екрана: екран лише гаситься (0x28, див. DspCore::sleep),
    а не присипляється — інакше сенсор не сканує. Будить дотик: сенсор опитуємо
    раз на 0,2 с (короткий вихід зі сну, одне читання I2C), і ще будить спад INT,
    якщо сенсор його дає. Будильник — той самий таймер, до свого строку.
    Прокинувшись, радіо однаково стартує заново: короткий глибокий сон на 50 мс —
    і далі звичайний шлях, як після пробудження EXT0.
    26.10.2026, звіт власника «из сна при касании не выходит»: екран спав разом
    із сенсором (0x10), а сон чекав лише INT і читав тільки байт стану — сенсор,
    чий звіт не дочитано, замовкав.  */
void YoExtras::_sleepUntilTouch(){
  extern YoST77922Touch ts;
  /*  палець ще на кнопці «вимкнути»: чекаємо, поки сенсор перестане бачити дотик  */
  for(int i = 0; i < 300; i++){ ts.read(); if(!ts.isTouched) break; delay(10); }
  delay(300);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  pinMode(TS_INT, INPUT_PULLUP);
  bool intWake = digitalRead(TS_INT) == HIGH;    /* лінія, що вже в нулі, будила б без кінця */
  if(intWake){ gpio_wakeup_enable((gpio_num_t)TS_INT, GPIO_INTR_LOW_LEVEL); esp_sleep_enable_gpio_wakeup(); }
  const int64_t sl = _alarmSleepSecs();
  const int64_t alarmAt = sl > 0 ? esp_timer_get_time() + sl * 1000000LL : -1;
  uint32_t rep0 = ts.reports;
  for(;;){
    int64_t wait = 200000;
    if(alarmAt > 0){
      const int64_t left = alarmAt - esp_timer_get_time();
      if(left <= 0){ s_softTouchWake = 0; break; }      /* будильник: s_alarmWakeArmed уже 1 */
      if(left < wait) wait = left;
    }
    esp_sleep_enable_timer_wakeup((uint64_t)wait);
    Serial.flush();
    esp_light_sleep_start();
    ts.read();                                     /* звіт дочитується весь — сенсор не замовкає */
    if(ts.isTouched || ts.reports != rep0){ s_softTouchWake = 1; s_alarmWakeArmed = 0; break; }
    rep0 = ts.reports;
    if(intWake && digitalRead(TS_INT) == LOW){     /* INT тримається в нулі без дотику — будитимемо лише таймером */
      gpio_wakeup_disable((gpio_num_t)TS_INT); esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO); intWake = false;
    }
  }
  if(intWake) gpio_wakeup_disable((gpio_num_t)TS_INT);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_timer_wakeup(50000);
  esp_deep_sleep_start();
}
#endif
