#include "options.h"
#include "player.h"
#include "config.h"
#include "telnet.h"
#include "display.h"
#include "sdmanager.h"
#include "netserver.h"
#include "timekeeper.h"
#include "../extras/yoExtras.h"
#include "../extras/yoDsp.h"
#include "../extras/yoAirplay.h"
#include "../extras/yoDlna.h"
#include "../displays/tools/l10n.h"
#include "../pluginsManager/pluginsManager.h"
#if I2S_ES8311
#include "../ES8311/yoES8311.h"
#endif
#ifdef USE_NEXTION
#include "../displays/nextion.h"
#endif
Player player;
QueueHandle_t playerQueue;

#if VS1053_CS!=255 && !I2S_INTERNAL
  #if VS_HSPI
    Player::Player(): Audio(VS1053_CS, VS1053_DCS, VS1053_DREQ, &SPI2) {}
  #else
    Player::Player(): Audio(VS1053_CS, VS1053_DCS, VS1053_DREQ, &SPI) {}
  #endif
  void ResetChip(){
    pinMode(VS1053_RST, OUTPUT);
    digitalWrite(VS1053_RST, LOW);
    delay(30);
    digitalWrite(VS1053_RST, HIGH);
    delay(100);
  }
#else
  #if !I2S_INTERNAL
    Player::Player() {}
  #else
    Player::Player(): Audio(true, I2S_DAC_CHANNEL_BOTH_EN)  {}
  #endif
#endif


void Player::init() {
  Serial.print("##[BOOT]#\tplayer.init\t");
  playerQueue=NULL;
  _resumeFilePos = 0;
  _hasError=false;
  playerQueue = xQueueCreate( 10, sizeof( playerRequestParams_t ) );
  setOutputPins(false);
  delay(50);
#if defined(MQTT_ROOT_TOPIC) || defined(YO_BROWSEURL)
  memset(burl, 0, MQTT_BURL_SIZE);
#endif
  if(MUTE_PIN!=255) pinMode(MUTE_PIN, OUTPUT);
  #if I2S_DOUT!=255
    #if !I2S_INTERNAL
      #if I2S_MCLK!=255
        setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_DIN, I2S_MCLK);   /* DIN — мікрофон кодека */
      #else
        setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
      #endif
      #if I2S_ES8311
        /*  Кодеку нужен уже идущий MCLK, поэтому его настраиваем после
            того, как выводы I2S розданы.                                  */
        if(!es8311_begin(getSampleRate()?getSampleRate():16000))
          Serial.println("##[ERROR]#\tES8311 не настроен, звука не будет");
      #endif
    #endif
  #else
    SPI.begin();
    if(VS1053_RST>0) ResetChip();
    begin();
  #endif
  setBalance(config.store.balance);
  setTone(config.store.bass, config.store.middle, config.store.trebble);
  setVolume(0);
  _status = STOPPED;
  _volTimer=false;
  //randomSeed(analogRead(0));
  #if PLAYER_FORCE_MONO
    forceMono(true);
  #endif
  _loadVol(config.store.volume);
  setConnectionTimeout(CONNECTION_TIMEOUT, CONNECTION_TIMEOUT_SSL);
  Serial.println("done");
}

void Player::sendCommand(playerRequestParams_t request){
  if(playerQueue==NULL) return;
  /*  Щосекундні службові команди (перевірка картки, поріг індикатора) — лише в майже порожню
      чергу. Поки радіо стартує, головний цикл чергу ще не розбирає, і за кілька секунд вони
      займали всі місця: «грати після ввімкнення» губилось — радіо після ввімкнення мовчало
      (26.10.2026, видно з журналу черги).  */
  if((request.type == PR_CHECKSD || request.type == PR_VUTONUS) && uxQueueMessagesWaiting(playerQueue) >= 2) return;
  if(xQueueSend(playerQueue, &request, PLQ_SEND_DELAY) != pdTRUE)
    Serial.printf("##PLAYER#\tчерга плеєра повна — команду %d загублено\n", (int)request.type);
}

void Player::resetQueue(){
  if(playerQueue!=NULL) xQueueReset(playerQueue);
}

void Player::stopInfo() {
  config.setSmartStart(0);
  netserver.requestOnChange(MODE, 0);
}

void Player::setError(){
  _hasError=true;
  config.setTitle(config.tmpBuf);
  telnet.printf("##ERROR#:\t%s\n", config.tmpBuf);
}

void Player::setError(const char *e){
  strlcpy(config.tmpBuf, e, sizeof(config.tmpBuf));
  setError();
}

void Player::_stop(bool alreadyStopped, bool keepAmp){
  log_i("%s called", __func__);
  _extRelease(true);
  if(remoteStationName && _status == PLAYING){   /* запам'ятати місце в проповіді */
    uint32_t p = posSec();
    burlResume = (burlDur && p + 5 >= burlDur) ? 0 : p;
  }
  if(config.getMode()==PM_SDCARD && !alreadyStopped) config.sdResumePos = player.getFilePos();
  _status = STOPPED;
  if(!keepAmp) setOutputPins(false);
  if(!_hasError) config.setTitle((display.mode()==LOST || display.mode()==UPDATING)?"":LANG::const_PlStopped);
  config.station.bitrate = 0;
  config.setBitrateFormat(BF_UNKNOWN);
  #ifdef USE_NEXTION
    nextion.bitrate(config.station.bitrate);
  #endif
  setDefaults();
  if(!alreadyStopped) stopSong();
  yoDsp.fadeReset();
  netserver.requestOnChange(BITRATE, 0);
  display.putRequest(DBITRATE);
  display.putRequest(PSTOP);
  //setDefaults();
  //if(!alreadyStopped) stopSong();
  if(!lockOutput) stopInfo();
  if (player_on_stop_play) player_on_stop_play();
  pm.on_stop_play();
}

void Player::initHeaders(const char *file) {
  if(strlen(file)==0 || true) return; //TODO Read TAGs
  connecttoFS(sdman,file);
  eofHeader = false;
  while(!eofHeader) Audio::loop();
  //netserver.requestOnChange(SDPOS, 0);
  setDefaults();
}
void resetPlayer(){
  if(!config.store.watchdog) return;
  player.resetQueue();
  player.sendCommand({PR_STOP, 0});
  player.loop();
}

#ifndef PL_QUEUE_TICKS
  #define PL_QUEUE_TICKS 0
#endif
#ifndef PL_QUEUE_TICKS_ST
  /*  Скільки чекати на черзі, коли нічого не грає. Дотик опитується раз на
      оберт головного циклу, тож довге чекання = рідкий опит пальця й ривки
      при прокручуванні списків.  */
  #define PL_QUEUE_TICKS_ST 4
#endif
void Player::loop() {
  cachePos();                 /* знімок позиції для сторінки — лише звідси */
  if(playerQueue==NULL) return;
  playerRequestParams_t requestP;
  if(xQueueReceive(playerQueue, &requestP, (isRunning() || _extData)?PL_QUEUE_TICKS:PL_QUEUE_TICKS_ST)){
    switch (requestP.type){
      case PR_STOP: _fadeOutWait(); _stop(); break;
      case PR_PLAY: {
        Serial.printf("##PLAYER#\tграти %d\n", (int)requestP.payload);
        _fadeOutWait();
        if (requestP.payload>0) {
          config.setLastStation((uint16_t)requestP.payload);
        }
        _play((uint16_t)abs(requestP.payload)); 
        if (player_on_station_change) player_on_station_change(); 
        pm.on_station_change();
        break;
      }
      case PR_TOGGLE: {
        toggle();
        break;
      }
      case PR_VOL: {
        config.setVolume(requestP.payload);
        Audio::setVolume(volToI2S(requestP.payload));
        break;
      }
      #ifdef USE_SD
      case PR_CHECKSD: {
        if(config.getMode()==PM_SDCARD){
          if(!sdman.cardPresent()){
            sdman.stop();
            config.changeMode(PM_WEB);
          }
        }
        break;
      }
      #endif
      case PR_VUTONUS: {
        if(config.vuThreshold>10) config.vuThreshold -=10;
        break;
      }
      case PR_EXT: {
        if(requestP.payload) _extStart();
        else if(extOn){ _extRelease(false); _stop(); }
        break;
      }
      case PR_SEEK: {
        _seekAt = 0;                    /* виконали — далі показуємо справжню позицію */
        /*  Перемотка файла картки йде саме тут, у головному циклі, а не з тієї
            задачі, що попросила. setFilePos скидає буфер і стан декодера
            (MP3/FLAC), і поки це робила задача веб-сервера, головний цикл у
            той самий час декодував із тих самих буферів: звук спотикався, а
            від частих перемоток радіо зависало й сторож його перезавантажував.  */
        if(isRunning() && requestP.payload > 0) setFilePos((uint32_t)requestP.payload);
        break;
      }
      case PR_BURL: {
      #if defined(MQTT_ROOT_TOPIC) || defined(YO_BROWSEURL)
        if(strlen(burl)>0){
          _fadeOutWait();
          browseUrl();
        }
      #endif
        break;
      }
          
      default: break;
    }
  }
  Audio::loop();
  if(extOn) _extPump();
  /*  Розмір файлу проповіді — із першого, повного з'єднання  */
  if(remoteStationName && !burlRanged && !burlSize && yoContentLen()) burlSize = yoContentLen();
  if(!isRunning() && _status==PLAYING && !extOn) _stop(true);
  if(_volTimer){
    if((millis()-_volTicks)>3000){
      config.saveVolume();
      _volTimer=false;
    }
  }
  /*
#ifdef MQTT_ROOT_TOPIC
  if(strlen(burl)>0){
    browseUrl();
  }
#endif*/
}

void Player::setOutputPins(bool isPlaying) {
  if(REAL_LEDBUILTIN!=255) digitalWrite(REAL_LEDBUILTIN, LED_INVERT?!isPlaying:isPlaying);
  bool _ml = MUTE_LOCK?!MUTE_VAL:(isPlaying?!MUTE_VAL:MUTE_VAL);
  if(extras.s.dac) _ml = MUTE_VAL;       /* звук іде на зовнішній ЦАП — вбудований підсилювач вимкнено */
  if(MUTE_PIN!=255) digitalWrite(MUTE_PIN, _ml);
}

/*  Перемотка файла картки: у чергу, а показуємо одразу цільову позицію.  */
void Player::seekTo(uint32_t pos){
  _seekTo = pos;
  _seekAt = millis();
  sendCommand({PR_SEEK, (int)pos});
}

/*  Знімок позиції — лише з головного циклу, раз на чверть секунди.  */
void Player::cachePos(){
  const uint32_t now = millis();
  if(now - _cAt < 250) return;
  _cAt = now;
  if(!isRunning() || config.getMode() != PM_SDCARD){ _cPos = _cSize = _cTime = _cDur = _cFill = 0; return; }
  _cPos  = getFilePos();
  _cSize = getFileSize();
  _cTime = getAudioCurrentTime();
  _cDur  = getAudioFileDuration();
  _cFill = inBufferFilled();
}

uint32_t Player::shownFilePos(){
  /*  Поки команда чекає в черзі, кажемо ту позицію, про яку просили: інакше
      програма встигає отримати відповідь зі старою, і повзунок відкочується.  */
  if(_seekAt && millis() - _seekAt < 1500) return _seekTo;
  /*  А далі — позиція ЗВУКУ, а не читання файла. getFilePos() показує, доки
      дочитали, а між ним і динаміком лежить повний вхідний буфер (сотня з
      лишком кілобайт — секунд шість на 128 кбіт/с). Одразу після перемотки
      читання мчить уперед, набираючи буфер, і повзунок стрибав туди ж, хоч
      звук ішов із потрібного місця. Віднімаємо те, що ще не відтворене.  */
  const uint32_t p = _cPos, q = _cFill;
  const uint32_t v = p > q ? p - q : 0;
  return v < sd_min ? sd_min : v;
}

/*  Перемикання й зупинка — через затихання: звук ще 0,3 с грає, стишуючись,
    і лише тоді обривається. Новий потік (станція, трек, проповідь) наростає
    за 0,7 с від першого відліку.  */
void Player::_fadeOutWait(){
  if(_status != PLAYING || (!isRunning() && !extOn)) return;
  yoDsp.fadeOut(300);
  uint32_t t0 = millis();
  while(!yoDsp.faded() && millis() - t0 < 500){ if(extOn) _extPump(); else Audio::loop(); vTaskDelay(1); }
}

/*  ---------- AirPlay ----------
    Звук приходить уже готовими відліками (extras/yoAirplay): тут лише
    перемкнути плеєр і подавати їх у ту саму обробку, що й потоки.  */
void Player::_extStart(){
  if(extOn) return;
  if(_status == PLAYING){ _fadeOutWait(); _stop(false, true); }   /* підсилювач лишаємо — не клацне */
  else if(isRunning()) stopSong();
  remoteStationName = false;
  _hasError = false;
  extFormat(44100);
  _loadVol(config.store.volume);
  yoDsp.fadeIn(300);
  extOn = true;
  _extData = false;
  _status = PLAYING;
  dlna.stopped();                         /* колонку DLNA теж замінено */
  config.setDspOn(1);
  char t[160];
  if(!airplay.takeTitle(t, sizeof(t))) strlcpy(t, airplay.device()[0] ? airplay.device() : "AirPlay", sizeof(t));
  config.setTitle(t);
  netserver.requestOnChange(MODE, 0);
  setOutputPins(true);
  display.putRequest(PSTART);
  if (player_on_start_play) player_on_start_play();
  pm.on_start_play();
  Serial.printf("##AIRPLAY#\tграє з «%s»\n", airplay.device());
}

void Player::_extRelease(bool byRadio){
  if(!extOn) return;
  _fadeOutWait();
  extOn = false;
  _extData = false;
  if(byRadio) airplay.released();
}

void Player::_extPump(){
  /*  Скільки пакетів за раз: поки I2S бере без очікування (буфер драйвера
      ще не повний) — більше, щойно запис почав чекати — досить.  */
  static int16_t buf[(352 + 4) * 2];
  _extData = false;
  const uint32_t t0 = millis();
  for(uint8_t k = 0; k < 6; k++){
    const size_t n = airplay.read(buf, 353);
    if(!n) break;
    _extData = true;
    for(size_t i = 0; i < n; i++){
      int16_t s[2] = { buf[i * 2], buf[i * 2 + 1] };
      extSample(s);
    }
    if(millis() - t0 > 6) break;
  }
  char t[160];
  if(airplay.takeTitle(t, sizeof(t))) config.setTitle(t);
}

void Player::fadeStop(){
  if(_status != PLAYING) return;
  _fadeOutWait();
  _stop(false, true);                     /* підсилювач лишаємо: слідом грає нове джерело, інакше клацне */
}

void Player::_play(uint16_t stationId) {
  log_i("%s called, stationId=%d", __func__, stationId);
  _extRelease(true);                      /* грав AirPlay — станція забирає звук собі */
  yoDsp.fadeIn(700);
  _hasError=false;
  setDefaults();
  _status = STOPPED;
  /*  Підсилювач тут не вимикаємо: при перемиканні станцій він клацав двічі
      (вимкнули — увімкнули після з'єднання). Звук і так уже стишено; не
      з'єдналось — _stop() вимкне.  */
  remoteStationName = false;
  
  if(!config.prepareForPlaying(stationId)) return;
  _loadVol(config.store.volume);
  
  bool isConnected = false;
  if(config.getMode()==PM_SDCARD && SDC_CS!=255){
    isConnected=connecttoFS(sdman,config.station.url,config.sdResumePos==0?_resumeFilePos:config.sdResumePos-player.sd_min);
  }else {
    config.saveValue(&config.store.play_mode, static_cast<uint8_t>(PM_WEB));
  }
  connproc = false;
  if(config.getMode()==PM_WEB) isConnected=connecttohost(config.station.url);
  connproc = true;
  if(isConnected){
    _status = PLAYING;
    config.configPostPlaying(stationId);
    /*  Тиша між станціями (з'єднались, але звуку ще нема: буфер тільки набирається) — час
        записати налаштування у флеш: запис зупиняє обидва ядра на 9–94 мс, посеред звуку це
        провал (див. Config::eepromLoop). Саме тут — щоб уже з остаточними «остання станція»
        й «грати після ввімкнення»: інакше після вимкнення живлення радіо само не заграло б.  */
    config.commitNow();
    extras.saveNow();
    setOutputPins(true);
    if (player_on_start_play) player_on_start_play();
    pm.on_start_play();
  }else{
    telnet.printf("##ERROR#:\tError connecting to %.128s\n", config.station.url);
    snprintf(config.tmpBuf, sizeof(config.tmpBuf), "Не вдалося з'єднатися"); setError();
    _stop(true);
  };
}

#if defined(MQTT_ROOT_TOPIC) || defined(YO_BROWSEURL)
void Player::browseUrl(){
  _extRelease(true);
  yoDsp.fadeIn(700);
  _hasError=false;
  /*  Перемотка всередині проповіді — теж новий запит; тоді «що грало до
      неї» не переписуємо, інакше після кінця проповіді вмикалось би радіо,
      навіть якщо до проповіді воно мовчало.  */
  bool wasRemote = remoteStationName;
  remoteStationName = true;
  config.setDspOn(1);
  if(!wasRemote) resumeAfterUrl = burlResumeRadio >= 0 ? burlResumeRadio : (_status==PLAYING);
  burlResumeRadio = -1;
  display.putRequest(PSTOP);
  /*  підсилювач не вимикаємо — перемотка проповіді клацала б  */
  config.setTitle(LANG::const_PlConnect);
  if (connecttohost(burl)){
    _status = PLAYING;
    config.setTitle(burlTitle);          /* у проповіді — ім'я проповідника */
    netserver.requestOnChange(MODE, 0);
    setOutputPins(true);
    display.putRequest(PSTART);
    if (player_on_start_play) player_on_start_play();
    pm.on_start_play();
  }else{
    telnet.printf("##ERROR#:\tError connecting to %.128s\n", burl);
    snprintf(config.tmpBuf, sizeof(config.tmpBuf), "Не вдалося з'єднатися"); setError();
    _stop(true);
  }
  //memset(burl, 0, MQTT_BURL_SIZE);
}
#endif

/*  Плейлист не закільцьований: перший і останній пункти — це краї, далі
    по колу не йдемо. Кінець останнього треку просто зупиняє відтворення,
    бо обробник кінця файлу теж кличе next().  */
void Player::prev() {
  uint16_t lastStation = config.lastStation();
  if(config.getMode()==PM_WEB || !config.store.sdsnuffle){
    if (lastStation <= 1) return;                 /* перший — раніше нікуди */
    config.lastStation(lastStation-1);
  }
  sendCommand({PR_PLAY, config.lastStation()});
}

void Player::next() {
  uint16_t lastStation = config.lastStation();
  if(config.getMode()==PM_WEB || !config.store.sdsnuffle){
    if (lastStation >= config.playlistLength()) return;   /* останній — це кінець */
    config.lastStation(lastStation+1);
  }else{
    config.lastStation(random(1, config.playlistLength()));
  }
  sendCommand({PR_PLAY, config.lastStation()});
}

void Player::toggle() {
  if (_status == PLAYING) {
    sendCommand({PR_STOP, 0});
  } else if (remoteStationName && burl[0]) {
    /*  Зупинили проповідь — дотик продовжує її з того ж місця. Раніше
        тут завжди вмикалась остання станція.  */
    burlSeek(burlResume);
  } else {
    sendCommand({PR_PLAY, config.lastStation()});
  }
}

void Player::burlSeek(uint32_t sec) {
  if(sec && burlSize && burlDur){
    if(sec >= burlDur) sec = burlDur > 5 ? burlDur - 5 : 0;
    yoRangeFrom = (uint32_t)((uint64_t)burlSize * sec / burlDur);
    burlRanged = true;
  }else{
    sec = 0; yoRangeFrom = 0; burlRanged = false;
  }
  burlBase = sec;
  sendCommand({PR_BURL, 0});
}

uint32_t Player::posSec() {
  if(remoteStationName && burlDur) return burlBase + getAudioCurrentTime();
  return getAudioCurrentTime();
}

uint32_t Player::durSec() {
  if(remoteStationName && burlDur) return burlDur;
  return getAudioFileDuration();
}

void Player::stepVol(bool up) {
  if (up) {
    if (config.store.volume <= 254 - config.store.volsteps) {
      setVol(config.store.volume + config.store.volsteps);
    }else{
      setVol(254);
    }
  } else {
    if (config.store.volume >= config.store.volsteps) {
      setVol(config.store.volume - config.store.volsteps);
    }else{
      setVol(0);
    }
  }
}

uint8_t Player::volToI2S(uint8_t volume) {
  int vol = map(volume, 0, 254 - config.station.ovol * 3 , 0, 254);
  if (vol > 254) vol = 254;
  if (vol < 0) vol = 0;
  return vol;
}

void Player::_loadVol(uint8_t volume) {
  if(volOverride >= 0) volume = (uint8_t)volOverride;   /* див. player.h */
  setVolume(volToI2S(volume));
}

void Player::setVol(uint8_t volume) {
  _volTicks = millis();
  _volTimer = true;
  player.sendCommand({PR_VOL, volume});
}
