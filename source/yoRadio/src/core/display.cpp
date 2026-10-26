#include "Arduino.h"
#include "options.h"
#include "../extras/yoHang.h"
#include "WiFi.h"
#include "config.h"
#include "display.h"
#include "network.h"
#include "netserver.h"
#include "timekeeper.h"
#include "player.h"
#include "../pluginsManager/pluginsManager.h"
#include "../displays/dspcore.h"
#include "../displays/tools/l10n.h"
#include "../menu/yoMenu.h"
#include "../m2/m2player.h"
#include "../m2/m2update.h"
#include "../m2/m2pages.h"
#include "../extras/yoExtras.h"
#include "../extras/yoSplash.h"
#include "../extras/yoSfx.h"
#include "../extras/yoSpectrum.h"

Display display;

#ifndef CORE_STACK_SIZE
  #define CORE_STACK_SIZE  1024*5   /* було 4К: у задачі екрана лишалось 924 Б запасу — замало */
#endif
#ifndef DSP_TASK_PRIORITY
  #define DSP_TASK_PRIORITY  4
#endif
#ifndef DSP_TASK_CORE_ID
  #define DSP_TASK_CORE_ID  0
#endif
#ifndef DSP_TASK_DELAY
  #define DSP_TASK_DELAY pdMS_TO_TICKS(10) // cap for 50 fps
#endif
#define DSP_QUEUE_TICKS 0
#ifndef DSQ_SEND_DELAY
  #define DSQ_SEND_DELAY  pdMS_TO_TICKS(200)
#endif

QueueHandle_t displayQueue = NULL;

#ifdef YO_DEBUG
uint32_t yoDspN = 0, yoDspMax = 0, yoDspFrom = 0, yoDspDraw = 0, yoDspNet = 0;
uint32_t yoMenuMs = 0, yoFadeMs = 0;   /* скільки триває саме меню й наплив */
char     yoDspWhat[24] = {0};          /* найдовший крок відмальовки */
uint32_t yoDspWhatMs = 0;
#define DSTEP(call) { uint32_t _s0 = millis(); call; uint32_t _sd = millis() - _s0; \
                      if(_sd > yoDspWhatMs){ yoDspWhatMs = _sd; strlcpy(yoDspWhat, #call, sizeof(yoDspWhat)); } }
#else
#define DSTEP(call) call
#endif

static void loopDspTask(void * pvParameters){
  while(true){
    yoHbDsp++;                                   /* сторож зависань (extras/yoHang) */
    timekeeper.loop0();                          /* хід годинника, RSSI, перевірка картки — ядро 0 (загубилось при переписуванні екрана) */
    if(yoHangTestMs){ uint32_t ms = yoHangTestMs; yoHangTestMs = 0; Serial.printf("##HANG#\tперевірка: задача екрана спить %u с\n", (unsigned)(ms / 1000)); vTaskDelay(pdMS_TO_TICKS(ms)); }
    if(displayQueue==NULL) break;
#ifdef YO_DEBUG
    /*  Скільки встигає задача екрана й хто в ній довгий: саме тут і дотик
        «відстає від пальця», навіть коли головний цикл летить.  */
    {
      static uint32_t prev = 0;
      uint32_t t0 = millis();
      if(!yoDspFrom) yoDspFrom = t0;
      if(prev){ uint32_t d = t0 - prev; if(d > yoDspMax) yoDspMax = d; }
      prev = t0; yoDspN++;
      display.loop();
      uint32_t t1 = millis(); if(t1 - t0 > yoDspDraw) yoDspDraw = t1 - t0;
    #ifndef NETSERVER_LOOP1
      netserver.loop();
      uint32_t t2 = millis(); if(t2 - t1 > yoDspNet) yoDspNet = t2 - t1;
    #endif
    }
#else
    display.loop();
  #ifndef NETSERVER_LOOP1
    netserver.loop();
  #endif
#endif
    /*  Щойно вивели кадр (прокрутка, перехід, хвиля) — наступний без сну:
        10 мс між обертами самі по собі обмежували прокрутку до ~30 кадрів.
        Нічого не рухається — спимо, як і раніше. Але зовсім не віддавати ядро
        не можна: прокрутка без пауз не пускала задачу простою, і сторож задач
        перезавантажував радіо. Тож між кадрами — 2 мс, а раз на чверть секунди руху — 8.  */
    {
      static uint32_t seen = 0, busyT = 0;
      const uint32_t f = g_m2Frames, now = millis();
      if(f != seen){
        if(!busyT) busyT = now;
        if(now - busyT > 250){ vTaskDelay(pdMS_TO_TICKS(8)); busyT = now; }
        else vTaskDelay(pdMS_TO_TICKS(2));
      }else{ busyT = 0; vTaskDelay(DSP_TASK_DELAY); }
      seen = f;
    }
  }
  vTaskDelete( NULL );
}

void Display::_createDspTask(){
  xTaskCreatePinnedToCore(loopDspTask, "DspTask", CORE_STACK_SIZE,  NULL,  DSP_TASK_PRIORITY, NULL, DSP_TASK_CORE_ID);
}

DspCore dsp;

void Display::init() {
  Serial.print("##[BOOT]#\tdisplay.init\t");
  _bootStep = 0;
  dsp.initDisplay();
  displayQueue = xQueueCreate( 5, sizeof( requestParams_t ) );
  while(displayQueue==NULL){;}
  _createDspTask();
  Serial.println("done");
}

uint16_t Display::width(){ return dsp.width(); }
uint16_t Display::height(){ return dsp.height(); }

/*  Поки радіо шукає мережу — анімована заставка з розділу ресурсів; вимкнена
    чи немає файла — логотип.  */
void Display::_bootScreen(){
  dsp.fillScreen(config.theme.background);
  if(!(extras.s.splashOff == 0 && !YoExtras::wokeForAlarm() && splash.begin())) dsp.drawLogo(bootLogoTop);
  _bootStep = 1;
}

/*  Мережі немає. Власної точки доступу радіо не піднімає, тож одразу список
    мереж — у ньому людина й вибирає свою.  */
void Display::_noNetScreen() {
  dsp.fillScreen(config.theme.background);
  yomenu.openWifi(true);
}

/*  Плавна зміна екранів — тим самим способом, що й у меню: гасимо підсвітку,
    міняємо картинку в темряві, засвічуємо назад.  */
void Display::_beginFade(displayMode_e to){
  if(_fadeStep >= 0){ _swichMode(to); return; }   /* зміна вже триває */
  _fadeTo = to; _fadeStep = 0; _fadeTick = 0;
}

void Display::fadeLoop(){
  if(_fadeStep < 0) return;
  uint32_t now = millis();
  if(now - _fadeTick < 12) return;
  _fadeTick = now;
  const int8_t STEPS = 8;
#if BRIGHTNESS_PIN!=255
  /*  «Повна» — та, що має бути зараз: уночі нічна, а не денна.  */
  uint16_t full = extras.pwmTarget();
#endif
  if(_fadeStep < STEPS){
#if BRIGHTNESS_PIN!=255
    if(config.store.dspon) analogWrite(BRIGHTNESS_PIN, full - full*(_fadeStep+1)/STEPS);
#endif
    if(++_fadeStep == STEPS) _swichMode(_fadeTo);   /* міняємо у темряві */
  }else{
    int8_t k = _fadeStep - STEPS + 1;
#if BRIGHTNESS_PIN!=255
    if(config.store.dspon) analogWrite(BRIGHTNESS_PIN, full*k/STEPS);
#else
    (void)k;
#endif
    if(++_fadeStep >= 2*STEPS){
      _fadeStep = -1;
#if BRIGHTNESS_PIN!=255
      if(config.store.dspon) extras.pwmSet(full);
#endif
    }
  }
}

void Display::forceRedraw(){
  if (network.status != CONNECTED && network.status != SDREADY){ _noNetScreen(); return; }
  if (!_ensurePlayer()) return;
  _mode = PLAYER;
  m2::P.setStatus(0);
  m2::P.show();
  m2::P.render();                    /* увесь кадр одразу — поки не засвітилась підсвітка */
}

void Display::_start() {
  /*  З карткою пам'яті готовність настає майже одразу — мережі чекати не
      треба, і заставка обривалась, не доказавши вступ (без картки радіо
      чекало Wi-Fi, і вона встигала). Даємо їй доказати, але не довше ніж
      5 с від увімкнення — інакше плеєр чекав би на несправній заставці.  */
  /*  Ліміт — від першого кадру заставки, а не від увімкнення: ES3C35P стартує довше, і
      «не довше 5 с від увімкнення» обривало заставку на півдорозі або не давало їй початись
      (власник 26.10.2026: «звук запуску, а заставка — частково або зовсім ні»).  */
  if(splash.worthWaiting()){ _startWait = true; return; }
  if(_startWait || splash.active()) Serial.printf("##DSP#\tстарт плеєра на %u мс, вступ заставки %s\n", (unsigned)millis(), splash.introDone() ? "доказано" : "обірвано");
  _startWait = false;
  splash.stop();
  if (network.status != CONNECTED && network.status != SDREADY) {
    _noNetScreen();
    _bootStep = 2;
    return;
  }
  _finishStart(true);
}

/*  Стартували без мережі — тоді замість плеєра лише список мереж. Мережа
    з'явилась (вибрали в меню чи радіо саме повернулось у збережену) — плеєр
    показуємо тут, у задачі екрана, перед будь-яким малюванням.  */
bool Display::_ensurePlayer(){
  if(_playerBuilt) return true;
  if(_bootStep != 2 || (network.status != CONNECTED && network.status != SDREADY)) return false;
  Serial.println("##DSP#\tмережа з'явилась після старту без неї — показую плеєр");
  _finishStart(!yomenu.active());      /* меню відкрите — намалюється, коли воно закриється */
  return true;
}

void Display::_finishStart(bool draw){
  _playerBuilt = true;
  _mode = PLAYER;
  config.setTitle(LANG::const_PlReady);
  if(draw){
    m2::P.show();
    m2::P.render();
  }
  _bootStep = 2;
  if(_lostPending){ _lostPending = false; if(network.linkLost && WiFi.status() != WL_CONNECTED) putRequest(NEWMODE, LOST); }
  pm.on_display_player();
}

void Display::_swichMode(displayMode_e newmode) {
  /*  Старих діалогів yoRadio (гучність, сон, номер станції, «налаштування»)
      немає: лишається головний екран — усе це він показує сам.  */
  if (newmode == VOL || newmode == SLEEPING || newmode == NUMBERS || newmode == INFO ||
      newmode == SETTINGS || newmode == TIMEZONE || newmode == WIFI) newmode = PLAYER;
  if (newmode == STATIONS){ m2::stationsRequest(); return; }       /* список — сторінка меню */
  if (newmode == _mode || (network.status != CONNECTED && network.status != SDREADY)) return;
  if (!_ensurePlayer()) return;
  /*  Зв'язок може зникнути ще на заставці завантаження — до готового плеєра
      режими не міняємо, стан мережі наздожене її власний цикл.  */
  if (_bootStep != 2){ if(newmode == LOST) _lostPending = true; return; }
  _mode = newmode;
  if (newmode == SCREENSAVER || newmode == SCREENBLANK){
    /*  Заставка — просто згаслий екран (годинника на весь екран, як у старому
        вигляді, більше немає); дотик його засвітить.  */
    config.isScreensaver = true;
    m2::P.hide();
    config.setDspOn(false, false);
    return;
  }
  config.screensaverTicks = SCREENSAVERSTARTUPDELAY;
  config.screensaverPlayingTicks = SCREENSAVERSTARTUPDELAY;
  if (config.isScreensaver){ config.isScreensaver = false; config.setDspOn(config.store.dspon, false); }
  /*  «немає зв'язку», «картка», «оновлення» — картка на головному екрані  */
  if (newmode == LOST || newmode == SDCHANGE || newmode == UPDATING){
    m2::P.setStatus(newmode == LOST ? 1 : (newmode == SDCHANGE ? 2 : 3));
    m2::P.show();
    m2::P.invalAll();
    m2::P.render();
    return;
  }
  if (newmode == CLEAR){
    m2::P.hide();
    dsp.fillScreen(config.theme.background);
    return;
  }
  /*  PLAYER  */
  m2::P.setStatus(0);
  numOfNextStation = 0;
  config.setDspOn(config.store.dspon, false);
  m2::P.show();
  m2::P.render();                    /* перехід іде в темряві — кадр готовий до того, як засвітиться */
  pm.on_display_player();
}

void Display::openStationsNow(){ m2::stationsRequest(); }
void Display::forceLogo(){ m2::P.reloadLogo(); }

void Display::resetQueue(){
  if(displayQueue!=NULL) xQueueReset(displayQueue);
}

void Display::putRequest(displayRequestType_e type, int payload){
  if(displayQueue==NULL) return;
  /*  список станцій — сторінка меню (m2/m2stations.cpp)  */
  if(type == NEWMODE && payload == STATIONS){ m2::stationsRequest(); return; }
  requestParams_t request;
  request.type = type;
  request.payload = payload;
  xQueueSend(displayQueue, &request, DSQ_SEND_DELAY);
}

void Display::loop() {
  if(_bootStep==0) { _bootScreen(); return; }
  if(displayQueue==NULL || _locked) return;
  /*  Іде оновлення з GitHub — екран належить його ходу, хоч би що було відкрите.  */
  if(m2::otaViewActive()){
    m2::otaViewRender();
    requestParams_t drop;
    while(xQueueReceive(displayQueue, &drop, 0)) { }
    return;
  }
  if(m2::otaViewEnded() && _bootStep == 2) forceRedraw();       /* не вийшло — назад на плеєр */
  if(_splashDemoMs){
    _splashDemoUntil = millis() + _splashDemoMs; _splashDemoMs = 0;
    dsp.fillScreen(0);
    bool ok = splash.begin(true);
    Serial.printf("##DSP#\tзаставка: %s\n", ok ? "почато" : "файл не відкрився");
    if(!ok) _splashDemoUntil = 0;
    else sfx.test(SFX_START);
  }
  if(_splashDemoUntil){
    /*  Кнопка «показати заставку» в меню: меню закривається в темряві й засвічує
        підсвітку вже після — без цього заставка йшла б на чорному екрані.  */
    if(yomenu.fading()) yomenu.render();
    if(millis() < _splashDemoUntil && splash.active()){ splash.tick(); return; }
    _splashDemoUntil = 0; splash.stop(); forceRedraw();
    return;
  }
  if(!_playerBuilt) _ensurePlayer();
  /*  Такт плавної зміни — до всіх дострокових виходів: якщо піти раніше,
      зміна застрягне на погашеній підсвітці.  */
#ifdef YO_DEBUG
  { uint32_t f0 = millis(); fadeLoop(); uint32_t d = millis() - f0; if(d > yoFadeMs) yoFadeMs = d; }
#else
  fadeLoop();
#endif
  /*  Поки відкрите меню, решта мовчить, а черга просто спорожнюється — інакше
      накопичені запити вивалились би на екран разом, щойно меню закриють.  */
  if(yomenu.active() || yomenu.fading()){
    /*  Поки відкрите меню, рисок спектра не видно — отже й складати відліки
        в його буфер нема потреби: у звуковому тракті це робота на кожен
        відлік. Назад увімкне сам екран плеєра, коли меню закриють.  */
    yoSpec.wanted = false;
#ifdef YO_DEBUG
    { uint32_t m0 = millis(); yomenu.render(); uint32_t d = millis() - m0; if(d > yoMenuMs) yoMenuMs = d; }
#else
    yomenu.render();
#endif
    requestParams_t drop;
    while(xQueueReceive(displayQueue, &drop, 0)) { }
    return;
  }
  if(m2::P.shown()){
    if(_redrawReq && !fading()){ _redrawReq = false; m2::P.invalAll(); }
    DSTEP(m2::P.render());
  }
  if(_bootStep == 1 && splash.active()) splash.tick();     /* заставка, поки радіо шукає мережу */
  /*  Старт відклали, щоб заставка доказала вступ — повертаємось до нього.  */
  if(_startWait && !splash.worthWaiting()) _start();

  requestParams_t request;
  if(xQueueReceive(displayQueue, &request, DSP_QUEUE_TICKS)){
    /*  Плеєра ще немає (старт без мережі) — лише старт.  */
    if(!_ensurePlayer() && request.type != DSP_START) return;
    bool pm_result = true;
    pm.on_display_queue(request, pm_result);
    if(!pm_result) return;
    switch (request.type){
      case NEWMODE: {
        /*  Повернення на плеєр — плавне, звідки б не йшло; службові екрани
            (оновлення, втрата зв'язку) міняються одразу.  */
        displayMode_e nm = (displayMode_e)request.payload;
        bool svc = (_mode==LOST || _mode==UPDATING || _mode==CLEAR || _mode==SLEEPING ||
                    nm==LOST || nm==UPDATING || nm==CLEAR || nm==SLEEPING);
        bool smooth = !svc && nm==PLAYER && _mode!=PLAYER;
        if(smooth) _beginFade(nm); else _swichMode(nm);
        break;
      }
      case CLOSEPLAYLIST: player.sendCommand({PR_PLAY, request.payload}); break;   /* кнопки/енкодер: грати вибрану */
      case SDFILEINDEX: if(_mode == SDCHANGE) m2::P.setStatusCount(request.payload); break;
      case DSP_START: _start(); break;
#if LIGHT_SENSOR!=255
      case CLOCK:
        if(config.store.dspon){ config.store.brightness = AUTOBACKLIGHT(analogRead(LIGHT_SENSOR)); config.setBrightness(); }
        break;
#endif
      /*  Решту (назва, гучність, бітрейт, погода, годинник…) головний екран бере
          сам зі стану радіо — запити старих віджетів лише прибираємо з черги.  */
      default: break;
    }
  }
}

void Display::flip(){ dsp.flip(); }
void Display::invert(){ dsp.invert(); }

bool Display::deepsleep(){
#if BRIGHTNESS_PIN!=255
  dsp.sleep();
  return true;
#else
  return false;
#endif
}

void Display::wakeup(){
#if BRIGHTNESS_PIN!=255
  dsp.wake();
#endif
}
