#include "options.h"
#include <ESPmDNS.h>
#include "time.h"
#include "rtcsupport.h"
#include "network.h"
#include "../extras/yoExtras.h"
#include <esp_wifi.h>
#include "display.h"
#include "config.h"
#include "telnet.h"
#include "netserver.h"
#include "../extras/yoDlna.h"
#include "../extras/yoAirplay.h"
#include "player.h"
#include "../extras/yoVersion.h"
#include "../extras/yoSfx.h"
#include "timekeeper.h"
#include "../pluginsManager/pluginsManager.h"

#ifndef WIFI_ATTEMPTS
  #define WIFI_ATTEMPTS  16
#endif

#ifndef SEARCH_WIFI_CORE_ID
  #define SEARCH_WIFI_CORE_ID  0
#endif
MyNetwork network;

/*  Відладка «наче мережі немає»: переживає програмне перезавантаження.  */
static RTC_NOINIT_ATTR uint32_t _noNetBoot;
static const uint32_t NONET_MAGIC = 0x4E4F4E54;
void MyNetwork::skipBootWifi(){ _noNetBoot = NONET_MAGIC; }

void MyNetwork::WiFiReconnected(WiFiEvent_t event, WiFiEventInfo_t info){
  Serial.printf("##WIFI#\tадреса %s%s\n", WiFi.localIP().toString().c_str(),
                network.lostPlaying ? " — до обриву грало, вмикаю знову" : "");
  if(network._try == TRY_RUN) network._try = TRY_OK;
  network.beginReconnect = false;
  network.linkLost = false;
  network._reTry = 0;
  network._reAt = 0;
  /*  Підключились не до тієї, що була: запам'ятовуємо, щоб наступного разу
      радіо починало саме з неї.  */
  if(network._reCur >= 0){
    if(config.store.lastSSID != network._reCur + 1) config.setLastSSID(network._reCur + 1);
    network._reCur = -1;
  }
  player.lockOutput = false;
  delay(100);
  display.putRequest(NEWMODE, PLAYER);
  /*  «Грало до обриву» — одноразове: раніше прапорець так і лишався, і
      кожна наступна подія «адресу отримано» (роутер оновлює її сам, через
      години) знову вмикала станцію, хоч її давно зупинили.  */
  if(millis() > 15000) sfx.play(SFX_CONNECT);     /* на старті мережа — не подія: там свій звук */
  bool resume = network.lostPlaying;
  network.lostPlaying = false;
  if(config.getMode()==PM_SDCARD) {
    network.status=CONNECTED;
    display.putRequest(NEWIP, 0);
  }else{
    display.putRequest(NEWMODE, PLAYER);
    display.putRequest(NEWIP, 0);        /* повертаємо адресу замість підказки */
    if (resume) player.sendCommand({PR_PLAY, config.lastStation()});
  }
  #ifdef MQTT_ROOT_TOPIC
    connectToMqtt();
  #endif
}

void MyNetwork::WiFiLostConnection(WiFiEvent_t event, WiFiEventInfo_t info){
  /*  Спроба, яку замовила людина: одразу кажемо, чому не вийшло.  */
  if(network._try == TRY_RUN){
    uint8_t r = info.wifi_sta_disconnected.reason;
    network._tryEv = true;
    Serial.printf("##WIFI#\t%s: відмова %u (%s)\n", network._tryS, (unsigned)r,
                  WiFi.disconnectReasonName((wifi_err_reason_t)r));
    /*  Перша відмова часто хибна: маршрутизатор буває зайнятий, а драйвер
        одразу каже «не той пароль». Одну спробу робимо мовчки ще раз.  */
    if(network._tryAgain < 1){                  /* і «не видно» теж: після паузи пошуку модуль буває ще не на тому каналі */
      network._tryAgain++;
      network._tryAt = millis();
      WiFi.begin(network._tryS, network._tryP);
      return;
    }
    if(r == WIFI_REASON_NO_AP_FOUND)            network._try = TRY_NOTFOUND;
    else if(r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
            r == WIFI_REASON_HANDSHAKE_TIMEOUT || r == WIFI_REASON_AUTH_EXPIRE ||
            r == WIFI_REASON_MIC_FAILURE)        network._try = TRY_BADPASS;
    else                                         network._try = TRY_FAIL;
    return;                      /* нічого не зупиняємо: ми ще не були в мережі */
  }
  /*  Спроба повернутись у збережену мережу не вдалась — пишемо чому: без
      цього «не підключається» нема з чого розбирати.  */
  if(network._reCur >= 0 && network._reCur < config.ssidsCount){
    uint8_t r = info.wifi_sta_disconnected.reason;
    Serial.printf("##WIFI#\t%s: не вдалось повернутись, відмова %u (%s)\n", config.ssids[network._reCur].ssid, (unsigned)r,
                  WiFi.disconnectReasonName((wifi_err_reason_t)r));
  }
  if(!network.beginReconnect && network.status != SOFT_AP){
    sfx.play(SFX_ERROR);
    Serial.printf("Lost connection, reconnecting to %s...\n", config.store.lastSSID ? config.ssids[config.store.lastSSID-1].ssid : "?");
    if(config.getMode()==PM_SDCARD) {
      network.status=SDREADY;
      display.putRequest(NEWIP, 0);
    }else{
      network.lostPlaying = player.isRunning();
      if (network.lostPlaying) { player.lockOutput = true; player.sendCommand({PR_STOP, 0}); }
      display.putRequest(NEWMODE, LOST);
    }
  }
  network.beginReconnect = true;
  /*  Тут раніше стояв WiFi.reconnect(). Кожна невдала спроба знову кидала
      цю саму подію — виходило нескінченне коло без жодної паузи: ядро теж
      саме перепідключалось, радіомодуль не встигав нічого, пошук мереж не
      запускався зовсім, а меню й звук стояли. Тепер лише позначаємо втрату,
      а спроби робить loop() — по одній, з паузами.  */
  network.linkLost = true;
}

bool MyNetwork::wifiBegin(bool silent){
  uint8_t ls = (config.store.lastSSID == 0 || config.store.lastSSID > config.ssidsCount) ? 0 : config.store.lastSSID - 1;
  uint8_t startedls = ls;
  uint8_t errcnt = 0;
  WiFi.mode(WIFI_STA);
  /*
  char buf[MDNS_LENGTH];
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
  if(strlen(config.store.mdnsname)>0){
    WiFi.setHostname(config.store.mdnsname);
  }else{
    snprintf(buf, MDNS_LENGTH, "yoradio-%x", config.getChipId());
    WiFi.setHostname(buf);
  }
  */
  while (true) {
    if(!silent){
      Serial.printf("##[BOOT]#\tAttempt to connect to %s\n", config.ssids[ls].ssid);
      Serial.print("##[BOOT]#\t");
      display.putRequest(BOOTSTRING, ls);
    }
    WiFi.disconnect(true, true); //disconnect & erase internal credentials https://github.com/e2002/yoradio/pull/164/commits/89d8b4450dde99cd7930b84bb14d81dab920b879
    delay(100);
    WiFi.begin(config.ssids[ls].ssid, config.ssids[ls].password);
    while (WiFi.status() != WL_CONNECTED) {
      if(!silent) Serial.print(".");
      delay(500);
      if(REAL_LEDBUILTIN!=255 && !silent) digitalWrite(REAL_LEDBUILTIN, !digitalRead(REAL_LEDBUILTIN));
      errcnt++;
      /*  Мережі просто немає в ефірі або пароль не той — чекати решту восьми
          секунд нема чого, пробуємо наступну збережену.  */
      if(errcnt > 6 && (WiFi.status() == WL_NO_SSID_AVAIL || WiFi.status() == WL_CONNECT_FAILED)) errcnt = WIFI_ATTEMPTS + 1;
      if (errcnt > WIFI_ATTEMPTS) {
        errcnt = 0;
        ls++;
        if (ls > config.ssidsCount - 1) ls = 0;
        if(!silent) Serial.println();
        break;
      }
    }
    if (WiFi.status() != WL_CONNECTED && ls == startedls) {
      return false; break;
    }
    if (WiFi.status() == WL_CONNECTED) {
      config.setLastSSID(ls + 1);
      return true; break;
    }
  }
  return false;
}

/*  Остання надія, коли список мереж зник: сам драйвер Wi-Fi тримає назву й
    пароль останньої мережі в NVS. Підключаємось за ними й одразу кладемо
    мережу назад у список — інакше радіо пішло б у точку доступу, звідки без
    пароля не вибратись.  */
bool MyNetwork::wifiRemembered(){
  if(Config::wifiWiped()) return false;      /* мережі прибрали свідомо */
  WiFi.mode(WIFI_STA);
  wifi_config_t c;
  if(esp_wifi_get_config(WIFI_IF_STA, &c) != ESP_OK || !c.sta.ssid[0]) return false;
  BOOTLOG("wifi list is empty, trying remembered %s", (const char*)c.sta.ssid);
  WiFi.begin();
  for(uint8_t i = 0; i < 24 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  if(WiFi.status() != WL_CONNECTED) return false;
  char line[110];
  snprintf(line, sizeof(line), "%s\t%s\n", (const char*)c.sta.ssid, (const char*)c.sta.password);
  config.saveWifiList(line);
  config.setLastSSID(1);
  BOOTLOG("wifi list restored from radio module: %s", (const char*)c.sta.ssid);
  return true;
}

void searchWiFi(void * pvParameters){
  if(!network.wifiBegin(true)){
    delay(10000);
    xTaskCreatePinnedToCore(searchWiFi, "searchWiFi", 1024 * 4, NULL, 0, NULL, SEARCH_WIFI_CORE_ID);
  }else{
    network.status = CONNECTED;
    netserver.begin(true);
    telnet.begin(true);
    network.setWifiParams();
    display.putRequest(NEWIP, 0);
  }
  vTaskDelete( NULL );
}

#define DBGAP false

void MyNetwork::begin() {
  BOOTLOG("network.begin");
  /*  Вимикаємо перш за все: інакше в режимі точки доступу ядро саме
      підключається до запам'ятованої мережі, раз за разом, без пауз —
      і забирає ядро 0 собі. Екран у цей час ледве повзе, а сторожовий
      таймер перезавантажує радіо.  */
  WiFi.setAutoReconnect(false);
  /*  Події підписуємо одразу, а не лише коли мережа знайшлась на старті:
      у режимі точки доступу setWifiParams() не викликався ніколи, і спроба
      підключитись із меню не мала як сказати ні про успіх, ні про відмову —
      просто мовчала до кінця очікування.  */
  WiFi.onEvent(WiFiReconnected, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(WiFiLostConnection, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  _evReady = true;
  config.initNetwork();
  if(_noNetBoot == NONET_MAGIC){
    _noNetBoot = 0;
    BOOTLOG("перевірка: стартую, наче жодної знайомої мережі поруч немає");
    _noNetwork();
    return;
  }
  if (config.ssidsCount == 0 && !DBGAP && wifiRemembered()) {
    /*  Список пропав, але драйвер Wi-Fi пам'ятає останню мережу сам —
        радіо лишається в мережі, а список ми тут-таки відновлюємо.  */
    status = CONNECTED;
    setWifiParams();
    Serial.println("##[BOOT]#\tdone");
    if (network_on_connect) network_on_connect();
    pm.on_connect();
    return;
  }
  if (config.ssidsCount == 0 || DBGAP) {
    _noNetwork();
    return;
  }
  if(config.getMode()!=PM_SDCARD){
    if(!wifiBegin()){
      _noNetwork();
      Serial.println("##[BOOT]#\tdone");
      return;
    }
    Serial.println(".");
    status = CONNECTED;
    setWifiParams();
  }else{
    status = SDREADY;
    xTaskCreatePinnedToCore(searchWiFi, "searchWiFi", 1024 * 4, NULL, 0, NULL, SEARCH_WIFI_CORE_ID);
  }
  
  Serial.println("##[BOOT]#\tdone");
  if(REAL_LEDBUILTIN!=255) digitalWrite(REAL_LEDBUILTIN, LOW);
  
#if RTCSUPPORTED
  if(config.isRTCFound()){
    rtc.getTime(&network.timeinfo);
    mktime(&network.timeinfo);
    display.putRequest(CLOCK);
  }
#endif
  if (network_on_connect) network_on_connect();
  pm.on_connect();
}

void MyNetwork::setWifiParams(){
  WiFi.setSleep(false);
  /*  Перепідключенням керуємо самі (loop()): у ядра воно без пауз і без
      переходу на інші збережені мережі.  */
  WiFi.setAutoReconnect(false);
  if(!_evReady){
    WiFi.onEvent(WiFiReconnected, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.onEvent(WiFiLostConnection, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    _evReady = true;
  }
  //config.setTimeConf(); //??
  mdnsStart();
}

/*  mDNS виносимо окремо. Раніше він піднімався лише якщо ім'я задане і
    MDNS.begin() вдався, а решта коду (AirPlay) зверталась до нього без
    перевірки — і будь-яка невдача mDNS означала падіння просто в setup(),
    тобто вічну перезавантажувальну петлю. Тепер: ім'я є завжди, невдача
    не страшна, а спробу можна повторити з головного циклу.  */
bool MyNetwork::mdnsStart(){
  if(mdnsReady) return true;
  if(WiFi.status() != WL_CONNECTED) return false;
  char buf[MDNS_LENGTH];
  const char* nm = config.store.mdnsname;
  if(strlen(nm) == 0){
    snprintf(buf, MDNS_LENGTH, "yoradio-%x", config.getChipId());
    nm = buf;
  }
  if(!MDNS.begin(nm)){
    Serial.println("##[BOOT]#\tmDNS не піднявся, спробуємо згодом");
    return false;
  }
  /*  Оголошуємо радіо в мережі, щоб програми-клієнти (Mac, Windows,
      Android) знаходили його самі, без введення адреси. Свій тип
      «_potuzhne._tcp» — лише наші радіо; «_http._tcp» — для браузерів.  */
  MDNS.setInstanceName("ПОТУЖНЕ РАДІО");
  MDNS.addService("http", "tcp", 80);
  MDNS.addService("potuzhne", "tcp", 80);
  MDNS.addServiceTxt("potuzhne", "tcp", "board", PR_BOARD);
  MDNS.addServiceTxt("potuzhne", "tcp", "ver", prVersion());
  MDNS.addServiceTxt("potuzhne", "tcp", "host", nm);
  /*  Задача mDNS від IDF створюється з пріоритетом 1 на ядрі 0 — нижче за
      все, що там працює (екран 4, мікрофон 3, AirPlay 5). Поки екран
      перемальовується, вона не встигає відповісти, і запит клієнта просто
      гине: заміром виходило, що радіо втрачає кожен четвертий запит, а
      відповідь іде 125–215 мс замість одиниць. Робота там мікроскопічна
      (кілька пакетів), тож піднімаємо її над малюванням — але значно нижче
      за lwIP (18) і Wi-Fi (23).  */
  if(TaskHandle_t h = xTaskGetHandle("mdns")) vTaskPrioritySet(h, 6);
  mdnsReady = true;
  Serial.printf("##[BOOT]#\tmDNS: %s.local\n", nm);
  return true;
}

void MyNetwork::requestTimeSync(bool withTelnetOutput, uint8_t clientId) {
  if (withTelnetOutput) {
    char timeStringBuff[50];
    strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    if (config.store.tzHour < 0) {
      telnet.printf(clientId, "##SYS.DATE#: %s%03d:%02d\n> ", timeStringBuff, config.store.tzHour, config.store.tzMin);
    } else {
      telnet.printf(clientId, "##SYS.DATE#: %s+%02d:%02d\n> ", timeStringBuff, config.store.tzHour, config.store.tzMin);
    }
  }
}

/*  Власної точки доступу більше немає — на прохання власника. Мережу
    вибирають на самому радіо, в меню, а точка лише заважала: у парі
    «точка + станція» обидві мусять сидіти на одному каналі, і підключення
    до мережі на іншому каналі зривалось навіть із правильним паролем.
    Стан лишається той самий (SOFT_AP = «мережі немає»), його знає решта коду. */
void MyNetwork::_noNetwork() {
  WiFi.setAutoReconnect(false);
  esp_wifi_disconnect();
  WiFi.mode(WIFI_STA);
  status = SOFT_AP;
  _bootNoNet = true;
  Serial.println("##[BOOT]#");
  BOOTLOG("************************************************");
  BOOTLOG("Мережі немає. Торкніться екрана радіо й виберіть її зі списку.");
  BOOTLOG("************************************************");
}

/*  Повернення в мережу: одна спроба за раз, пауза між ними росте, а
    збережені мережі перебираємо по черзі. Якщо тієї, що була, більше немає —
    радіо саме перейде на іншу знайому, і все це не чіпає ні звук, ні дотики.  */
void MyNetwork::loop(){
  /*  mDNS міг не піднятись на старті (мережа ще не встигла, не вистачило
      пам'яті). Без нього клієнти не знаходять радіо, тож тихо пробуємо
      знову раз на 10 с — і колонку AirPlay теж буде оголошено, бо її
      задача сама повторює реєстрацію служби.  */
  if(!mdnsReady && status == CONNECTED){
    static uint32_t mAt = 0;
    if(millis() - mAt > 10000){ mAt = millis(); mdnsStart(); }
  }
  /*  Не покладаємось лише на події: питаємо і сам стан станції.  */
  if(_try == TRY_RUN){
    wl_status_t st = WiFi.status();
    if(st == WL_CONNECTED)            _try = TRY_OK;
    /*  Стан станції після попередніх спроб (інших мереж) ще «мережі не видно» —
        віримо йому лише після відповіді саме на цю спробу або коли вона
        мовчить уже 10 с.  */
    else if(st == WL_NO_SSID_AVAIL && millis() - _tryAt > (_tryEv ? 4000UL : 10000UL))  _try = TRY_NOTFOUND;
    else if(st == WL_CONNECT_FAILED && millis() - _tryAt > (_tryEv ? 4000UL : 10000UL)) _try = TRY_BADPASS;
    else if(st == WL_DISCONNECTED && _tryAgain < 2 && millis() - _tryAt > 2500){
      /*  Модуль навіть не взявся за справу — просимо ще раз.  */
      _tryAgain++; _tryAt = millis();
      Serial.printf("##WIFI#\t%s: мовчить, прошу ще раз (%u)\n", _tryS, (unsigned)_tryAgain);
      WiFi.begin(_tryS, _tryP);
    }
    else if(millis() - _tryAt > 15000) _try = TRY_FAIL;          /* мовчить — годі чекати */
  }
  if(_try == TRY_OK && status != CONNECTED) _staUp();
  /*  Не вийшло — точку доступу повертаємо, щоб радіо не лишилось без нічого.  */
  if(_try == TRY_OK) _apWas = false;
  /*  Спроба скінчилась, а закрити її нікому: з екрана результату вийшли не
      кнопкою (назад, за часом) чи сторінка, що її почала, закрилась. Раніше
      така спроба лишалась назавжди — і радіо більше не поверталось ні в
      одну збережену мережу, аж до перезавантаження.  */
  if(_try >= TRY_OK){
    if(!_tryEndAt) _tryEndAt = millis() | 1;
    if(!tryHeld && millis() - _tryEndAt > 20000UL){
      Serial.printf("##WIFI#\tспробу «%s» ніхто не закрив — повертаюсь до збережених мереж\n", _tryS);
      tryClear();
    }
  }else _tryEndAt = 0;
  if(_try != TRY_NONE) return;               /* поки триває спроба людини — не заважаємо */
  if(status == SOFT_AP){
    /*  Стартували без мережі (роутер ще вантажився, радіо ввімкнули деінде) —
        раніше після цього радіо не пробувало жодної збереженої мережі, доки
        його не перезавантажать. Тепер пробує, як і після втрати зв'язку.  */
    if(config.ssidsCount == 0 || config.getMode() == PM_SDCARD) return;
    if(WiFi.status() == WL_CONNECTED){ _staUp(); return; }
  }else{
    if(!linkLost) return;
    if(WiFi.status() == WL_CONNECTED) return;          /* подія про адресу ось-ось прийде */
  }
  if(staPaused){
    /*  пауза на час пошуку мереж не може тривати вічно  */
    /*  Без мережі з самого старту список мереж відкритий сам, і людини біля
        радіо може не бути (роутер після вимкнення світла вантажиться довше
        за радіо) — тоді збережені пробуємо вже за 25 с, а не за три хвилини.  */
    uint32_t lim = (status == SOFT_AP && config.ssidsCount) ? 25000UL : 180000UL;
    if(millis() - _pauseAt > lim) pauseSta(false);
    return;
  }
  if((int32_t)(millis() - _reAt) < 0) return;
  /*  поки триває пошук мереж, підключатися нема чим: модуль один  */
  if(WiFi.scanComplete() == WIFI_SCAN_RUNNING){ _reAt = millis() + 2000; return; }
  if(config.ssidsCount == 0){ linkLost = false; return; }
  if(_reNext >= config.ssidsCount) _reNext = 0;
  _reCur = _reNext++;
  WiFi.disconnect(false, false);
  WiFi.begin(config.ssids[_reCur].ssid, config.ssids[_reCur].password);
  if(_reTry < 200) _reTry++;
  /*  перше коло по всіх збережених — швидко, далі рідше: і батарея, і ефір  */
  uint32_t pause = _reTry <= config.ssidsCount ? 9000 : (_reTry < 12 ? 20000 : 45000);
  _reAt = millis() + pause;
}

/*  Пошук мереж і спроби підключитися живуть в одному радіомодулі: поки
    йдуть спроби, скан не стартує взагалі. Тож на час пошуку спроби спиняємо.  */
void MyNetwork::pauseSta(bool on){
  if(staPaused == on) return;
  staPaused = on;
  _pauseAt = millis();
  if(on){
    /*  Обірвати треба саме незавершену спробу: доки вона триває, пошук мереж
        не запускається. WiFi.disconnect() тут не годиться — поки з'єднання ще
        не встановлене, він мовчки не робить нічого.  */
    /*  Але не спробу людини: екран «підключаюсь…» теж ставить паузу, і вона
        обривала щойно почате підключення — радіо писало «мережі не видно»
        навіть поруч із роутером.  */
    if(_try != TRY_RUN && WiFi.status() != WL_CONNECTED) esp_wifi_disconnect();
  }else{
    _reTry = 0;
    _reAt = millis() + 800;
  }
}

/*  Підключитись просто зараз: без перезавантаження й з негайною відповіддю.
    Відповідь дає подія: невірний пароль, мережі не видно чи вийшло.  */
/*  Спробу закрито. Якщо радіо не в мережі (спроба вибила його з тієї, де
    воно було, а нова не вдалась), повертаємось до збережених: раніше спроба
    не позначала втрату зв'язку, і радіо так і лишалось без мережі.  */
void MyNetwork::tryClear(){
  _try = TRY_NONE;
  if(WiFi.status() == WL_CONNECTED || status == SOFT_AP || config.ssidsCount == 0) return;
  linkLost = true;
  beginReconnect = true;
  _reTry = 0; _reNext = 0;
  _reAt = millis() + 500;
}

void MyNetwork::connectTo(const char* ssid, const char* pass){
  /*  грало — після підключення (нового чи повернення) заграє знову  */
  if(player.isRunning()) lostPlaying = true;
  strlcpy(_tryS, ssid, sizeof(_tryS));
  strlcpy(_tryP, pass ? pass : "", sizeof(_tryP));
  _try = TRY_RUN;
  _tryAt = millis();
  staPaused = false;
  WiFi.setAutoReconnect(false);
  /*  Своя точка доступу на час спроби йде геть: у парі «точка + станція»
      обидві мусять сидіти на одному каналі, і підключення до мережі на
      іншому каналі зривається. Не вийде — повернемо її назад.  */
  _tryAgain = 0;
  _tryEv = false;
  if(WiFi.getMode() != WIFI_STA){ WiFi.mode(WIFI_STA); delay(60); }
  /*  Поки триває пошук мереж, підключення просто не починається: радіомодуль
      зайнятий перебором каналів, і команду мовчки відкидають. Спершу пошук
      зупиняємо й чекаємо, доки він справді скінчиться.  */
  esp_wifi_scan_stop();
  for(uint8_t i = 0; i < 20 && WiFi.scanComplete() == WIFI_SCAN_RUNNING; i++) delay(50);
  WiFi.scanDelete();
  esp_wifi_disconnect();
  wl_status_t r = WiFi.begin(_tryS, _tryP);
  Serial.printf("##WIFI#\tспроба підключитись до %s (begin=%d)\n", _tryS, (int)r);
}

/*  Мережа з'явилась на ходу (були в точці доступу) — піднімаємо служби, як
    це робить пошук мережі при старті.  */
void MyNetwork::_staUp(){
  status = CONNECTED;
  linkLost = false; beginReconnect = false; _reTry = 0;
  if(_reCur >= 0){ if(config.store.lastSSID != _reCur + 1) config.setLastSSID(_reCur + 1); _reCur = -1; }
  Serial.printf("##WIFI#\tу мережі %s, адреса %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  if(!_staReady){
    _staReady = true;
    netserver.begin(true);
    telnet.begin(true);
    setWifiParams();
  }
  /*  Стартували без мережі — тоді setup() не дійшов ні до списку станцій, ні
      до «грати після ввімкнення». Доробляємо це тут, один раз.  */
  if(_bootNoNet){
    _bootNoNet = false;
    config.initPlaylistMode();
    player.lockOutput = false;
    if(config.store.smartstart == 1 && !YoExtras::wokeForAlarm()) player.sendCommand({PR_PLAY, config.lastStation()});   /* перед будильником — тихо */
    if (network_on_connect) network_on_connect();
    pm.on_connect();
  }
  dlna.begin();                      /* мережа з'явилась — колонку видно в ній */
  airplay.begin();
  display.putRequest(NEWIP, 0);
  display.putRequest(NEWMODE, PLAYER);
}

void MyNetwork::requestWeatherSync(){
  display.putRequest(NEWWEATHER);
}

void MyNetwork::dump(){
  static const char* ST[] = { "CONNECTED", "SOFT_AP", "FAILED", "SDREADY" };
  static const char* TR[] = { "NONE", "RUN", "OK", "BADPASS", "NOTFOUND", "FAIL" };
  uint32_t now = millis();
  Serial.printf("WSTATE status=%s wifi=%d ssid='%s' ip=%s linkLost=%d paused=%d(%lus) try=%s held=%d reTry=%u reNext=%u reCur=%d next=%lds bootNoNet=%d\n",
    status <= SDREADY ? ST[status] : "?", (int)WiFi.status(), WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
    linkLost, staPaused, staPaused ? (unsigned long)((now - _pauseAt) / 1000) : 0UL, _try <= TRY_FAIL ? TR[_try] : "?", tryHeld ? 1 : 0,
    (unsigned)_reTry, (unsigned)_reNext, (int)_reCur, (long)((int32_t)(_reAt - now) / 1000), _bootNoNet ? 1 : 0);
}
