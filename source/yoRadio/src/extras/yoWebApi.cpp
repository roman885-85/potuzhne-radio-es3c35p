#include "yoWebApi.h"
#include <SPIFFS.h>
#include <WiFi.h>
#include "esp_heap_caps.h"
#include "../AsyncWebServer/ESPAsyncWebServer.h"
#include "../core/options.h"
#include "../core/config.h"
#include "../core/player.h"
#include "../core/display.h"
#include "../core/network.h"
#include "../core/netserver.h"
#include "../core/sdmanager.h"
#include "yoExtras.h"
#include "yoDsp.h"
#include "yoMic.h"
#include "yoRecorder.h"
#include "yoSermons.h"
#include "yoLogos.h"
#include "yoSfx.h"
#include "yoOta.h"
#include <LittleFS.h>
#include "yoVersion.h"
#include "yoHang.h"
#include "yoDlna.h"
#include "yoAirplay.h"
#include "esp_core_dump.h"
#include "esp_flash.h"

/*  Коли веб-сервер востаннє віддавав файл (сторінку, логотип). Сторінка, що вантажиться, тримає
    кілька з'єднань, а з ними до 5,7 КБ внутрішньої пам'яті на кожне. Пошук логотипів (yoLogos.cpp)
    у цей час не починає запитів: 27.10.2026 разом вони вибирали внутрішню пам'ять до 3 КБ,
    і станція двічі втратила потік на 34 с.  */
volatile uint32_t yoWebHeavyAt = 0;
bool yoWebHeavyMark(AsyncWebServerRequest*){ yoWebHeavyAt = millis(); if(!yoWebHeavyAt) yoWebHeavyAt = 1; return true; }

/*  ---------- JSON у готовий буфер ----------
    Буфери лежать у PSRAM і живуть весь час: відповідь іде з них без копії
    (beginResponse_P на ESP32 — звичайний memcpy). Два браузери, що
    спитали одночасно, можуть отримати зіпсований JSON — сторінка такий
    просто пропустить і спитає знову за дві секунди.  */
struct JOut {
  char* b; size_t cap; size_t n = 0;
  JOut(char* buf, size_t c) : b(buf), cap(c) { b[0] = 0; }
  void put(char c){ if(n + 1 < cap){ b[n++] = c; b[n] = 0; } }
  void raw(const char* s){ while(*s) put(*s++); }
  /*  кома — перед усім, що йде не першим  */
  void sep(){ if(n){ char l = b[n-1]; if(l != '{' && l != '[' && l != ':' && l != ',') put(','); } }
  void str(const char* s){
    put('"');
    for(; *s; s++){
      uint8_t c = (uint8_t)*s;
      if(c == '"' || c == '\\'){ put('\\'); put((char)c); }
      else if(c < 0x20){ char e[8]; snprintf(e, sizeof(e), "\\u%04x", c); raw(e); }
      else put((char)c);
    }
    put('"');
  }
  void k(const char* key){ sep(); str(key); put(':'); }
  void num(long long v){ char t[24]; snprintf(t, sizeof(t), "%lld", v); raw(t); }
  void kn(const char* key, long long v){ k(key); num(v); }
  void ks(const char* key, const char* v){ k(key); str(v ? v : ""); }
  void obj(){ sep(); put('{'); }
  void arr(const char* key){ k(key); put('['); }
};

#define ST_CAP   8192
#define BIG_CAP  24576
static char* _stBuf = nullptr;      /* /api/state */
static char* _bigBuf = nullptr;     /* проповіді, мережі, логотипи */
static char* _recBuf = nullptr;     /* записи на картці — збирає головний цикл */

static void sendJson(AsyncWebServerRequest* r, const char* buf, size_t n){
  AsyncWebServerResponse* resp = r->beginResponse_P(200, "application/json", (const uint8_t*)buf, n);
  resp->addHeader("Cache-Control", "no-store");
  r->send(resp);
}

/*  ---------- черга команд ---------- */
struct WebCmd { char k[16]; char v[240]; };
static QueueHandle_t _q = nullptr;

static bool push(const char* k, const char* v){
  if(!_q) return false;
  WebCmd c;
  strlcpy(c.k, k, sizeof(c.k));
  strlcpy(c.v, v, sizeof(c.v));
  return xQueueSend(_q, &c, pdMS_TO_TICKS(50)) == pdTRUE;
}

/*  ---------- стан головного циклу, який читають обробники ---------- */
static const char* _msg = "";              /* відповідь на останню команду: помилка запису тощо */
static volatile bool _recReq = false;       /* сторінка чекає список записів */

#define SCAN_N 20
struct ScanItem { char s[33]; int8_t r; uint8_t e; };
static ScanItem  _scan[SCAN_N];
static volatile uint8_t _scanN = 0;
static volatile bool _scanning = false;
static uint32_t _scanT0 = 0;

/*  ---------- Wi-Fi: збережені мережі ---------- */
#define WF_MAX 5
static uint8_t readWifi(char ssid[][33], char pass[][65]){
  uint8_t n = 0;
  File f = SPIFFS.open(SSIDS_PATH, "r");
  if(!f) return 0;
  char line[112];
  while(f.available() && n < WF_MAX){
    size_t l = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[l] = 0;
    if(l && line[l-1] == '\r') line[--l] = 0;
    char* t = strchr(line, '\t');
    if(!t || t == line) continue;
    *t = 0;
    strlcpy(ssid[n], line, 33);
    strlcpy(pass[n], t + 1, 65);
    n++;
  }
  f.close();
  return n;
}


/*  Відбиток файлів сторінки: змінився — відкрита сторінка перезавантажиться
    сама, а не показуватиме старе, як було з головною yoRadio.  */
static uint32_t webStamp(){
  static uint32_t v = 0, t = 0;
  if(v && millis() - t < 5000) return v;
  uint32_t sum = 0;
  for(const char* f : { "/www/app.js.gz", "/www/app.css.gz" }){
    File h = SPIFFS.open(f, "r");
    if(h){ sum = sum * 31 + h.size(); h.close(); }
  }
  v = sum ? sum : 1; t = millis();
  return v;
}

/*  ---------- /api/hello: «ти ПОТУЖНЕ РАДІО?» для програм-клієнтів ---------- */
static void onHello(AsyncWebServerRequest* r){
  char b[400];
  JOut o(b, sizeof(b));
  o.put('{');
  o.kn("potuzhne", 1);
  o.ks("board", PR_BOARD);
  o.ks("v", prVersion());
  o.ks("build", prBuild());
  o.ks("host", config.store.mdnsname);
  o.ks("ip", WiFi.localIP().toString().c_str());
  o.ks("station", player.extOn ? "AirPlay" : player.remoteStationName && sermons.playing() >= 0 ? "проповідь" : config.station.name);
  o.kn("play", player.status() == PLAYING);
  o.put('}');
  AsyncWebServerResponse* resp = r->beginResponse(200, "application/json", b);
  resp->addHeader("Access-Control-Allow-Origin", "*");
  resp->addHeader("Cache-Control", "no-store");
  r->send(resp);
}

/*  ---------- /api/crash: останнє зависання чи падіння ----------
    Короткий зміст дампу — одразу; сам дамп (усі задачі зі стеками) — /api/crash.bin,
    розшифровувати esp-coredump з ELF тієї ж збірки.  */
static void onCrash(AsyncWebServerRequest* r){
  char b[900];
  JOut o(b, sizeof(b));
  o.put('{');
  o.ks("hang", YoHang::last()); o.kn("hangAt", YoHang::lastAt()); o.kn("thisBoot", YoHang::thisBoot());
  o.ks("rst", YoExtras::resetReason());
  size_t addr = 0, len = 0;
  const bool have = esp_core_dump_image_check() == ESP_OK && esp_core_dump_image_get(&addr, &len) == ESP_OK;
  o.kn("dump", have ? (long long)len : 0);
  if(have){
    esp_core_dump_summary_t* cs = (esp_core_dump_summary_t*)heap_caps_calloc(1, sizeof(esp_core_dump_summary_t), MALLOC_CAP_SPIRAM);
    if(cs && esp_core_dump_get_summary(cs) == ESP_OK){
      char t[24];
      o.ks("task", cs->exc_task);
      snprintf(t, sizeof(t), "0x%08lx", (unsigned long)cs->exc_pc); o.ks("pc", t);
      o.ks("elf", (const char*)cs->app_elf_sha256);
      o.arr("bt");
      for(uint32_t i = 0; i < cs->exc_bt_info.depth && i < 16; i++){ snprintf(t, sizeof(t), "0x%08lx", (unsigned long)cs->exc_bt_info.bt[i]); o.sep(); o.str(t); }
      o.put(']');
    }
    if(cs) free(cs);
  }
  o.put('}');
  AsyncWebServerResponse* resp = r->beginResponse(200, "application/json", b);
  resp->addHeader("Cache-Control", "no-store");
  r->send(resp);
}

static void onCrashBin(AsyncWebServerRequest* r){
  size_t addr = 0, len = 0;
  if(esp_core_dump_image_check() != ESP_OK || esp_core_dump_image_get(&addr, &len) != ESP_OK || !len || len > 0x40000){ r->send(404, "text/plain", "дампу немає"); return; }
  AsyncWebServerResponse* resp = r->beginResponse("application/octet-stream", len,
    [addr, len](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
      if(index >= len) return 0;
      size_t n = len - index; if(n > maxLen) n = maxLen; if(n > 4096) n = 4096;
      return esp_flash_read(esp_flash_default_chip, buf, addr + index, n) == ESP_OK ? n : 0;
    });
  resp->addHeader("Content-Disposition", "attachment; filename=\"coredump.bin\"");
  r->send(resp);
}

/*  ---------- /api/state ---------- */
static void onState(AsyncWebServerRequest* r){
  JOut o(_stBuf, ST_CAP);
  o.put('{');
  o.ks("v", prVersion());
  o.ks("build", prBuild());
  o.ks("fw", prMarker());
  o.ks("board", PR_BOARD);              /* сторінка «Про пристрій» пише, яка плата */
  o.ks("repo", PR_REPO);                /* звідки випуски — у кожної плати свій репозиторій */
  bool sta = WiFi.status() == WL_CONNECTED;
  o.ks("ip", sta ? WiFi.localIP().toString().c_str() : "");
  o.ks("ssid", sta ? WiFi.SSID().c_str() : "");
  o.kn("rssi", sta ? WiFi.RSSI() : 0);
  o.kn("heap", ESP.getFreeHeap());
  o.kn("heapBlk", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));   /* найбільший шматок — від нього залежить TLS */
  o.kn("psram", ESP.getFreePsram());
  o.kn("up", millis() / 1000);
  o.ks("rst", YoHang::thisBoot() ? "ЗАВИСАННЯ — радіо перезапустилось само" : YoExtras::resetReason());
  if(YoHang::last()[0]){ o.ks("hang", YoHang::last()); o.kn("hangAt", YoHang::lastAt()); }
  o.kn("hb", (long long)yoHbLoop); o.kn("hbDsp", (long long)yoHbDsp);     /* оберти циклу й екрана: зовні видно, що стоїть */
  o.kn("dlna", extras.s.dlnaOn ? 1 : 0);                                  /* бездротова колонка */
  o.kn("airplay", extras.s.airplayOn ? 1 : 0);                            /* колонка AirPlay */
  o.k("ap"); o.put('{'); o.kn("key", airplay.ready()); o.ks("err", airplay.keyState());
  o.kn("sess", airplay.session()); o.kn("on", player.extOn); o.ks("dev", airplay.device()); o.put('}');
  {
    char t[24];
    if(network.timeinfo.tm_year > 100){ strftime(t, sizeof(t), "%H:%M", &network.timeinfo); o.ks("time", t);
      strftime(t, sizeof(t), "%d.%m.%Y", &network.timeinfo); o.ks("date", t); }
  }
  o.kn("mode", config.getMode());
  o.kn("play", player.status() == PLAYING);
  o.kn("idx", config.lastStation());
  o.ks("name", player.extOn ? "AirPlay" : config.station.name);
  o.ks("title", config.station.title);
  o.ks("url", config.station.url);
  o.kn("vol", config.store.volume);
  o.kn("br", config.station.bitrate);
  o.ks("codec", player.status() == PLAYING ? player.getCodecname() : "");
  o.kn("bright", config.store.brightness);
  o.kn("snuffle", config.store.sdsnuffle);

  o.k("sleep"); o.put('{'); o.kn("set", extras.sleepMinutes()); o.kn("left", extras.sleepMinutes() ? extras.sleepLeftSec() : 0); o.put('}');
  const ExtStore& s = extras.s;
  o.k("alarm"); o.put('{'); o.kn("on", s.alarmOn); o.kn("h", s.alarmH); o.kn("m", s.alarmM); o.kn("days", s.alarmDays);
  o.kn("in", extras.alarmInMin()); o.kn("ring", extras.alarmRinging()); o.put('}');
  o.k("night"); o.put('{'); o.kn("on", s.nightOn); o.kn("from", s.nightFrom); o.kn("to", s.nightTo);
  o.kn("level", s.nightLevel); o.kn("act", extras.nightActive()); o.put('}');
  o.kn("led", s.ledMode);
  o.kn("save", s.batSave);
  o.k("dev"); o.put('{'); o.kn("noBat", s.noBat); o.kn("noIp", s.noIp); o.kn("noSd", s.noSd); o.kn("dac", s.dac); o.put('}');
  o.k("bat"); o.put('{'); o.kn("mv", extras.batMv()); o.kn("pct", extras.batPct()); o.kn("chg", extras.charging());
  o.kn("full", extras.charged()); o.kn("pwr", extras.onPower()); o.kn("low", extras.lowBattery()); o.put('}');
  o.k("rec"); o.put('{'); o.kn("on", recorder.active()); o.kn("sec", recorder.seconds()); o.kn("bytes", recorder.bytes());
  { const char* fn = recorder.fileName(); const char* sl = strrchr(fn, '/'); o.ks("file", sl ? sl + 1 : fn); }
  o.ks("err", recorder.lastError()); o.put('}');
  o.arr("fav");
  for(uint8_t i = 0; i < FAV_N; i++){ o.obj(); o.ks("n", extras.fav[i].name); o.ks("u", extras.fav[i].url); o.put('}'); }
  o.put(']');
  o.kn("favOn", extras.favPlaying());
  o.kn("favHide", s.favHide);
  o.kn("dst", s.dstMode);
  bool serm = player.remoteStationName && sermons.playing() >= 0;
  o.k("serm"); o.put('{'); o.kn("on", serm); o.kn("i", sermons.playing());
  if(serm){ const Sermon* it = sermons.at(sermons.playing()); if(it){ o.ks("t", it->title); o.ks("p", it->preacher); o.ks("c", it->cover); } }
  o.kn("pos", serm ? player.posSec() : 0); o.kn("dur", serm ? player.durSec() : 0);
  o.kn("n", sermons.count()); o.kn("load", sermons.loading()); o.kn("ver", sermons.version()); o.put('}');
  o.k("fs"); o.put('{'); o.kn("t", SPIFFS.totalBytes()); o.kn("u", SPIFFS.usedBytes()); o.put('}');
  o.kn("sdOk", sdman.ready);
  o.kn("logo", logos.version());
  o.kn("web", webStamp());
  o.ks("wfail", extras.wifiFail);
  /*  звук: еквалайзер і обробка  */
  o.k("snd"); o.put('{');
  o.kn("on", s.eqOn); o.kn("preset", s.eqPreset); o.kn("loud", s.eqLoud); o.kn("guard", s.eqGuard);
  o.kn("vb", s.vbass); o.kn("roomOn", s.eqRoomOn); o.kn("bal", config.store.balance);
  o.arr("eq");   for(uint8_t i = 0; i < EQ_BANDS; i++){ o.sep(); o.num(s.eq[i]); }     o.put(']');
  o.arr("room"); for(uint8_t i = 0; i < EQ_BANDS; i++){ o.sep(); o.num(s.eqRoom[i]); } o.put(']');
  o.kn("pre10", (long long)lroundf(yoDsp.preampDb() * 10));
  o.kn("us100", (long long)lroundf(yoDsp.usPerFrame() * 100));
  o.kn("rst", yoDsp.roomState()); o.kn("rpr", yoDsp.roomProgress()); o.ks("rmsg", yoDsp.roomMsg());
  o.put('}');
  /*  мікрофон  */
  o.k("mic"); o.put('{');
  o.kn("on", s.micOn); o.kn("gain", s.micGain); o.kn("play", s.micPlay); o.kn("run", mic.listening());
  o.kn("lvl", mic.levelDb()); o.kn("noise", mic.noiseDb()); o.kn("speech", mic.speech()); o.kn("aec", mic.aecActive());
  o.ks("heard", mic.heard() && millis() - mic.heardMs() < 15000 ? YoMic::gestureName(mic.heard()) : "");
  o.kn("clapOn", s.clapOn); o.kn("clapSens", s.clapSens);
  o.kn("clap2", YoMic::actionFor(MG_CLAP2)); o.kn("clap3", YoMic::actionFor(MG_CLAP3));
  o.kn("knockOn", s.knockOn); o.kn("knockSens", s.knockSens);
  o.kn("knock2", YoMic::actionFor(MG_KNOCK2)); o.kn("knock3", YoMic::actionFor(MG_KNOCK3));
  o.kn("ear", s.sleepEar); o.kn("earMin", s.sleepEarMin ? s.sleepEarMin : 10); o.kn("wake", s.presWake); o.kn("off", s.presOff);
  o.put('}');
  /*  звуки подій  */
  o.k("sfx"); o.put('{');
  o.kn("on", s.sfxOn); o.kn("vol", s.sfxVol); o.kn("mask", s.sfxMask); o.kn("fs", sfx.fsOk()); o.kn("user", sfx.userMask());
  o.kn("splashOff", s.splashOff); o.kn("splashVol", s.splashVol);
  o.put('}');
  /*  оновлення з GitHub: стан і хід (опис випуску — окремо, /api/ota)  */
  o.k("ota"); o.put('{');
  o.kn("st", ota.state()); o.kn("avail", ota.available()); o.ks("tag", ota.latest());
  o.kn("pct", ota.progress()); o.kn("got", ota.done()); o.kn("size", ota.total()); o.kn("bps", ota.speed());
  o.ks("step", ota.stepName()); o.ks("err", ota.error());
  o.put('}');
  o.ks("msg", _msg);
  o.put('}');
  sendJson(r, o.b, o.n);
}

/*  ---------- /api/eq ----------
    Розрахункова АЧХ ланцюга на 64 частотах (для графіка на сторінці) і
    останній замір мікрофоном, якщо був.  */
static void onEq(AsyncWebServerRequest* r){
  JOut o(_bigBuf, BIG_CAP);
  o.put('{');
  o.arr("resp");
  for(uint8_t i = 0; i < 64; i++){
    float f = 20.0f * pow(1000.0f, i / 63.0f);
    o.sep(); o.put('['); o.num(lroundf(f)); o.put(','); o.num(lroundf(yoDsp.responseDb(f) * 10)); o.put(']');
  }
  o.put(']');
  o.arr("sweep");
  if(mic.sweepState() == 2){
    for(uint8_t i = 0; i < YoMic::SWEEP_N; i++){
      float d = mic.sweepDb(i), nz = mic.sweepNoise(i);
      if(isnan(d)) continue;
      o.sep(); o.put('['); o.num(YoMic::sweepHz(i)); o.put(','); o.num(lroundf(d * 10)); o.put(','); o.num(lroundf(nz * 10)); o.put(']');
    }
  }
  o.put(']');
  o.kn("dsp", mic.sweepDsp());
  o.put('}');
  sendJson(r, o.b, o.n);
}

/*  ---------- /api/sermons ---------- */
/*  Увесь архів — понад 550 проповідей, до 200 КБ JSON: окремий буфер у PSRAM  */
#define SM_CAP  (340 * 1024)
static char* _smBuf = nullptr;
static void onSermons(AsyncWebServerRequest* r){
  if(!sermons.count() && !sermons.loading()) push("sermLoad", "1");
  if(!_smBuf) _smBuf = (char*)ps_malloc(SM_CAP);
  if(!_smBuf){ r->send(503, "application/json", "{\"err\":\"нема пам'яті\"}"); return; }
  JOut o(_smBuf, SM_CAP);
  o.put('{');
  o.ks("site", SERMON_SITE);
  o.kn("load", sermons.loading() || !sermons.count());
  o.kn("got", sermons.loadedSoFar());
  o.ks("err", sermons.error());
  o.kn("i", player.remoteStationName ? sermons.playing() : -1);
  o.arr("items");
  for(uint16_t i = 0; i < sermons.count(); i++){
    const Sermon* it = sermons.at(i);
    if(!it || o.n > SM_CAP - 600) break;
    o.obj(); o.ks("t", it->title); o.ks("p", it->preacher); o.ks("d", it->date);
    o.kn("dur", it->dur); o.ks("c", it->cover); o.ks("u", it->url); o.put('}');
  }
  o.put(']'); o.put('}');
  sendJson(r, o.b, o.n);
}

/*  ---------- /api/records: список збирає головний цикл ---------- */
static void recList(){
  JOut o(_recBuf, BIG_CAP);
  o.put('{');
  if(extras.s.noSd) o.ks("err", "функції картки вимкнено (розробник)");
  else if(!sdman.ready && !sdman.start()) o.ks("err", "картки немає");
  else{
    o.kn("free", sdman.totalBytes() - sdman.usedBytes());
    o.kn("total", sdman.totalBytes());
    o.arr("items");
    File d = sdman.open("/records");
    if(d && d.isDirectory()){
      File f;
      while((f = d.openNextFile())){
        if(!f.isDirectory()){
          const char* nm = f.name(); const char* sl = strrchr(nm, '/');
          o.obj(); o.ks("f", sl ? sl + 1 : nm); o.kn("s", f.size()); o.put('}');
        }
        f.close();
        if(o.n > BIG_CAP - 200) break;
      }
    }
    if(d) d.close();
    o.put(']');
    o.kn("rec", recorder.active());
  }
  o.put('}');
}

static void onRecords(AsyncWebServerRequest* r){
  _recReq = true;
  uint32_t t0 = millis();
  while(_recReq && millis() - t0 < 2500) vTaskDelay(pdMS_TO_TICKS(10));
  if(_recReq){ r->send(503, "application/json", "{\"err\":\"плата зайнята, спробуйте ще\"}"); return; }
  sendJson(r, _recBuf, strlen(_recBuf));
}

/*  Ім'я файлу запису: лише ім'я, без шляху — щоб не віддати нічого поза /records.  */
static bool safeName(const String& f){
  if(!f.length() || f.length() > 70) return false;
  if(f.indexOf('/') >= 0 || f.indexOf('\\') >= 0 || f.indexOf("..") >= 0) return false;
  return true;
}

static void onRecFile(AsyncWebServerRequest* r){
  if(!r->hasParam("f")){ r->send(400); return; }
  String f = r->getParam("f")->value();
  if(!safeName(f) || extras.s.noSd || !sdman.ready){ r->send(404, "text/plain", "немає"); return; }
  String path = "/records/" + f;
  if(!sdman.exists(path)){ r->send(404, "text/plain", "немає"); return; }
  const char* ct = f.endsWith(".mp3") ? "audio/mpeg" : f.endsWith(".aac") ? "audio/aac" :
                   f.endsWith(".flac") ? "audio/flac" : f.endsWith(".ogg") ? "audio/ogg" : "application/octet-stream";
  r->send(sdman, path, ct, r->hasParam("dl"));
}

/*  ---------- Wi-Fi ---------- */
/*  Підключення, замовлене сторінкою: що й чим закінчилось  */
static char     _wjS[33] = {0}, _wjP[65] = {0}, _wjIp[16] = {0};
static bool     _wjRun = false;
static uint8_t  _wjRes = 0;          /* n_Try_e */
static uint32_t _wjT = 0, _wjStartAt = 0;

static void onWifi(AsyncWebServerRequest* r){
  char ss[WF_MAX][33], pw[WF_MAX][65];
  uint8_t n = readWifi(ss, pw);
  JOut o(_bigBuf, BIG_CAP);
  o.put('{');
  o.ks("cur", WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "");
  o.arr("saved");
  for(uint8_t i = 0; i < n; i++){ o.sep(); o.str(ss[i]); }   /* паролі сторінці не віддаємо */
  o.put(']');
  o.kn("busy", _scanning);
  o.arr("scan");
  for(uint8_t i = 0; i < _scanN; i++){ o.obj(); o.ks("s", _scan[i].s); o.kn("r", _scan[i].r); o.kn("e", _scan[i].e); o.put('}'); }
  o.put(']');
  o.ks("fail", extras.wifiFail);
  o.k("join"); o.put('{'); o.ks("s", _wjS); o.kn("st", _wjRes); o.ks("ip", _wjIp); o.kn("ago", _wjT ? (millis() - _wjT) / 1000 : -1); o.put('}');
  o.put('}');
  sendJson(r, o.b, o.n);
}

static void onWifiJoin(AsyncWebServerRequest* r){
  if(!r->hasParam("ssid", true)){ r->send(400, "application/json", "{\"err\":\"немає назви мережі\"}"); return; }
  String v = r->getParam("ssid", true)->value();
  String p = r->hasParam("pass", true) ? r->getParam("pass", true)->value() : "";
  if(!v.length() || v.length() > 32 || p.length() > 64 || v.indexOf('\t') >= 0 || p.indexOf('\t') >= 0){
    r->send(400, "application/json", "{\"err\":\"задовга назва чи пароль\"}"); return;
  }
  push("wifiJoin", (v + "\t" + p).c_str());
  r->send(200, "application/json", "{\"ok\":1}");
}

/*  ---------- список станцій ---------- */
static File _plFile;
static bool _plOk = false;
static void onStationsBody(AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total){
  if(!index){
    _plOk = false;
    if(total > 60000 || config.getMode() != PM_WEB){ return; }
    _plFile = SPIFFS.open(TMP_PATH, "w");
  }
  if(_plFile) _plFile.write(data, len);
  if(index + len == total && _plFile){ _plFile.close(); _plOk = true; }
}
static void onStationsPost(AsyncWebServerRequest* r){
  if(config.getMode() != PM_WEB){ r->send(409, "application/json", "{\"err\":\"спершу перейдіть на радіо\"}"); return; }
  if(!_plOk){ r->send(400, "application/json", "{\"err\":\"список не дійшов\"}"); return; }
  _plOk = false;
  push("plSave", "1");
  r->send(200, "application/json", "{\"ok\":1}");
}

/*  ---------- логотипи ---------- */
static void onLogos(AsyncWebServerRequest* r){
  JOut o(_bigBuf, BIG_CAP);
  o.put('{');
  o.kn("v", logos.version());
  o.arr("f");
  /*  SPIFFS пласка: тек немає, є лише імена з «/logo/» усередині  */
  File d = SPIFFS.open("/");
  if(d){
    for(File f = d.openNextFile(); f; f = d.openNextFile()){
      const char* nm = f.path();
      if(strstr(nm, "/logo/") && (strstr(nm, ".565") || strstr(nm, ".jpg"))){ o.sep(); o.str(strrchr(nm, '/') + 1); }
      f.close();
      if(o.n > BIG_CAP - 40) break;
    }
    d.close();
  }
  o.put(']'); o.put('}');
  sendJson(r, o.b, o.n);
}

static bool hexName(const String& fn, char* crc){
  /*  «0a1b2c3d.565» — рівно 8 шістнадцяткових цифр і розширення  */
  if(fn.length() != 12 || !fn.endsWith(".565")) return false;
  for(uint8_t i = 0; i < 8; i++){ char c = fn[i]; if(!isxdigit((unsigned char)c)) return false; crc[i] = tolower(c); }
  crc[8] = 0;
  return true;
}

static void dropLogo(const char* crc){
  char p[32];
  static const char* ext[] = { "565", "jpg", "no" };
  for(const char* e : ext){ snprintf(p, sizeof(p), "/logo/%s.%s", crc, e); if(SPIFFS.exists(p)) SPIFFS.remove(p); }
}

static File _lgFile;
static char _lgCrc[9] = {0};
static size_t _lgLen = 0;
static void onLogoUpload(AsyncWebServerRequest* r, String filename, size_t index, uint8_t* data, size_t len, bool final){
  if(!index){
    _lgCrc[0] = 0; _lgLen = 0;
    if(!hexName(filename, _lgCrc)) return;
    _lgFile = SPIFFS.open("/logo/up.tmp", "w");
  }
  /*  рахуємо самі: size() відкритого на запис файлу SPIFFS віддає нуль  */
  if(_lgFile && len) _lgLen += _lgFile.write(data, len);
  if(final && _lgFile){
    size_t sz = _lgLen;
    _lgFile.close();
    if(sz == LOGO_S * LOGO_S * 2 && _lgCrc[0]){
      dropLogo(_lgCrc);
      char p[32]; snprintf(p, sizeof(p), "/logo/%s.565", _lgCrc);
      SPIFFS.rename("/logo/up.tmp", p);
      logos.bump();                          /* екран перечитає логотип */
    }else{ SPIFFS.remove("/logo/up.tmp"); _lgCrc[0] = 0; }
  }
}
/*  ---------- звуки подій: список і свої файли ---------- */
static void onSfxList(AsyncWebServerRequest* r){
  JOut o(_bigBuf, BIG_CAP);
  o.put('{');
  o.kn("fs", sfx.fsOk()); o.kn("total", (long long)sfx.fsTotal()); o.kn("used", (long long)sfx.fsUsed());
  o.arr("ev");
  for(uint8_t i = 0; i < SFX_N; i++){
    o.sep(); o.obj();
    o.ks("id", YoSfx::id((SfxEvent)i)); o.ks("t", YoSfx::title((SfxEvent)i));
    o.kn("user", (sfx.userMask() >> i) & 1); o.kn("ms", sfx.clipMs((SfxEvent)i));
    o.kn("vol", i == SFX_START ? extras.s.splashVol : (i < sizeof(extras.s.sfxEvVol) ? extras.s.sfxEvVol[i] : 100));
    o.put('}');
  }
  o.put(']');
  o.put('}');
  sendJson(r, o.b, o.n);
}

static File _sfFile;
static int  _sfEvent = -1;
static size_t _sfLen = 0;
static const char* _sfErr = nullptr;
static const size_t SFX_MAX_BYTES = 1024 * 1024;  /* сторінка шле моно 22 кГц 16 біт до 10 с (≈ 440 КБ); запас на свій WAV */

static void onSfxUpload(AsyncWebServerRequest* r, String filename, size_t index, uint8_t* data, size_t len, bool final){
  if(!index){
    _sfErr = nullptr; _sfLen = 0;
    _sfEvent = r->hasParam("e") ? YoSfx::find(r->getParam("e")->value().c_str()) : -1;
    if(_sfEvent < 0){ _sfErr = "невідома подія"; return; }
    if(!sfx.fsOk()){ _sfErr = "немає розділу ресурсів — прошийте радіо через USB"; return; }
    _sfFile = LittleFS.open("/snd/user/up.tmp", "w");
    if(!_sfFile){ _sfErr = "не вдалося записати файл"; return; }
  }
  if(_sfErr) return;
  if(_sfLen + len > SFX_MAX_BYTES){ _sfErr = "файл завеликий (до 1 МБ після перетворення)"; _sfFile.close(); LittleFS.remove("/snd/user/up.tmp"); return; }
  if(_sfFile && len) _sfLen += _sfFile.write(data, len);
  if(final && _sfFile){
    _sfFile.close();
    /*  перевіряємо заголовок: WAV, PCM 16 біт  */
    File f = LittleFS.open("/snd/user/up.tmp", "r");
    uint8_t h[44] = {0};
    size_t n = f ? f.read(h, sizeof(h)) : 0;
    if(f) f.close();
    bool ok = n >= 44 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4) && !memcmp(h + 12, "fmt ", 4)
              && h[20] == 1 && h[21] == 0 && h[34] == 16;
    if(!ok){ _sfErr = "потрібен WAV, PCM 16 біт"; LittleFS.remove("/snd/user/up.tmp"); return; }
    char p[40]; snprintf(p, sizeof(p), "/snd/user/%s.wav", YoSfx::id((SfxEvent)_sfEvent));
    LittleFS.remove(p);
    LittleFS.rename("/snd/user/up.tmp", p);
    sfx.reload((SfxEvent)_sfEvent);
    sfx.refreshUser();
  }
}
static void onSfxDone(AsyncWebServerRequest* r){
  if(_sfErr){ char b[120]; snprintf(b, sizeof(b), "{\"err\":\"%s\"}", _sfErr); r->send(400, "application/json", b); }
  else r->send(200, "application/json", "{\"ok\":1}");
}

static void onLogoDone(AsyncWebServerRequest* r){
  if(_lgCrc[0]) r->send(200, "application/json", "{\"ok\":1}");
  else r->send(400, "application/json", "{\"err\":\"потрібен файл 45x45\"}");
}

/*  ---------- команди ---------- */
static void onSet(AsyncWebServerRequest* r){
  size_t n = r->params();
  uint8_t ok = 0;
  for(size_t i = 0; i < n; i++){
    AsyncWebParameter* p = r->getParam(i);
    if(p->isFile()) continue;
    if(push(p->name().c_str(), p->value().c_str())) ok++;
  }
  r->send(ok ? 200 : 503, "application/json", ok ? "{\"ok\":1}" : "{\"err\":\"черга повна\"}");
}

void yoWebApiBegin(AsyncWebServer& s){
  if(!_q) _q = xQueueCreate(12, sizeof(WebCmd));
  if(!_stBuf)  _stBuf  = (char*)ps_malloc(ST_CAP);
  if(!_bigBuf) _bigBuf = (char*)ps_malloc(BIG_CAP);
  if(!_recBuf){ _recBuf = (char*)ps_malloc(BIG_CAP); if(_recBuf) strcpy(_recBuf, "{}"); }
  if(!_stBuf || !_bigBuf || !_recBuf) return;
  s.on("/api/hello",    HTTP_GET,  onHello);
  s.on("/api/state",    HTTP_GET,  onState);
  s.on("/api/crash.bin",HTTP_GET,  onCrashBin);
  s.on("/api/crash",    HTTP_GET,  onCrash);
  s.on("/api/set",      HTTP_ANY,  onSet);
  s.on("/api/eq",       HTTP_GET,  onEq);
  s.on("/api/sermons",  HTTP_GET,  onSermons);
  s.on("/api/records",  HTTP_GET,  onRecords);
  s.on("/api/rec",      HTTP_GET,  onRecFile);
  s.on("/api/wifi/join",HTTP_POST, onWifiJoin);
  s.on("/api/wifi",     HTTP_GET,  onWifi);
  s.on("/api/stations", HTTP_GET,  [](AsyncWebServerRequest* r){
    AsyncWebServerResponse* resp = r->beginResponse(SPIFFS, PLAYLIST_PATH, "text/plain; charset=utf-8");
    resp->addHeader("Cache-Control", "no-store");
    r->send(resp);
  });
  s.on("/api/stations", HTTP_POST, onStationsPost, nullptr, onStationsBody);
  s.on("/api/logos",    HTTP_GET,  onLogos);
  s.on("/api/logo",     HTTP_POST, onLogoDone, onLogoUpload);
  s.on("/api/sfx",      HTTP_GET,  onSfxList);
  s.on("/api/ota",      HTTP_GET,  [](AsyncWebServerRequest* r){
    /*  опис випуску — окремо від стану: він довгий  */
    static char* b = nullptr;
    if(!b) b = (char*)ps_malloc(2048);
    if(!b){ r->send(500); return; }
    JOut o(b, 2048);
    o.put('{'); o.ks("cur", prVersion()); o.ks("tag", ota.latest()); o.kn("avail", ota.available()); o.ks("notes", ota.notes()); o.put('}');
    sendJson(r, o.b, o.n);
  });
  s.on("/api/sfx",      HTTP_POST, onSfxDone, onSfxUpload);
  /*  логотипи — прямо з SPIFFS; сторінка додає ?v=<версія>, тож кеш на добу безпечний  */
  s.serveStatic("/logo/", SPIFFS, "/logo/").setCacheControl("max-age=86400").setFilter(yoWebHeavyMark);
}

/*  ---------- головний цикл ---------- */
static int clampi(const char* v, int lo, int hi){ int x = atoi(v); return x < lo ? lo : (x > hi ? hi : x); }

static void apply(const WebCmd& c){
  const char* k = c.k; const char* v = c.v;
  ExtStore& s = extras.s;
  bool ext = true;                       /* поле налаштувань — записати згодом */
  if(!strcmp(k, "sleep"))           { extras.setSleep(clampi(v, 0, 600)); ext = false; }
  else if(!strcmp(k, "alarmOn"))    s.alarmOn = clampi(v, 0, 1);
  else if(!strcmp(k, "sfxOn"))      s.sfxOn = clampi(v, 0, 1);
  else if(!strcmp(k, "splashOff"))  s.splashOff = clampi(v, 0, 1);
  else if(!strcmp(k, "splashVol"))  s.splashVol = clampi(v, 0, 100);
  else if(!strcmp(k, "splashDemo")) { display.splashDemo(clampi(v, 2000, 20000)); ext = false; }
  else if(!strcmp(k, "otaCheck"))   { ota.check(clampi(v, 0, 1) || extras.s.otaBeta); ext = false; }
  else if(!strcmp(k, "otaBeta"))    { extras.s.otaBeta = clampi(v, 0, 1); ota.check(extras.s.otaBeta); }
  else if(!strcmp(k, "otaInstall")) { ota.install(); ext = false; }
  else if(!strcmp(k, "sfxVol"))     s.sfxVol = clampi(v, 0, 100);
  else if(!strcmp(k, "sfxMask"))    s.sfxMask = clampi(v, 0, 0xFFFF);
  else if(!strcmp(k, "sfxEvVol")){
    /*  «подія:гучність», напр. «alarm:80»  */
    char id[16] = {0}; const char* c = strchr(v, ':');
    if(c && c - v < (int)sizeof(id)){
      memcpy(id, v, c - v);
      int e = YoSfx::find(id), g = clampi(c + 1, 0, 100);
      if(e == SFX_START) s.splashVol = g;
      else if(e >= 0 && e < (int)sizeof(s.sfxEvVol)) s.sfxEvVol[e] = g;
    }
  }
  else if(!strcmp(k, "crashClear")) { YoHang::clear(); ext = false; }
  else if(!strcmp(k, "dlna"))       { dlna.setOn(clampi(v, 0, 1) != 0); ext = false; }
  else if(!strcmp(k, "airplay"))    { airplay.setOn(clampi(v, 0, 1) != 0); ext = false; }
  else if(!strcmp(k, "hangTest"))   { yoHangTestMs = (uint32_t)clampi(v, 1, 180) * 1000UL; ext = false; }   /* перевірка сторожа: екран «зависне» на v с */
  else if(!strcmp(k, "sfxPlay"))    { int e = YoSfx::find(v); if(e >= 0) sfx.test((SfxEvent)e); ext = false; }
  else if(!strcmp(k, "sfxReset")){
    /*  свій звук прибрати — знову стандартний  */
    int e = YoSfx::find(v);
    if(e >= 0 && sfx.fsOk()){
      char p[40]; snprintf(p, sizeof(p), "/snd/user/%s.wav", YoSfx::id((SfxEvent)e));
      /*  файл могли саме читати (перша перевірка тривалості) — пробуємо кілька разів  */
      bool gone = false;
      for(uint8_t t = 0; t < 5 && !(gone = !LittleFS.exists(p) || LittleFS.remove(p)); t++) delay(40);
      if(!gone) Serial.printf("##SFX#\tне вдалося прибрати %s\n", p);
      sfx.reload((SfxEvent)e); sfx.refreshUser();
    }
    ext = false;
  }
  else if(!strcmp(k, "alarmH"))     s.alarmH = clampi(v, 0, 23);
  else if(!strcmp(k, "alarmM"))     s.alarmM = clampi(v, 0, 59);
  else if(!strcmp(k, "alarmDays"))  s.alarmDays = clampi(v, 0, 1);
  else if(!strcmp(k, "nightOn"))    s.nightOn = clampi(v, 0, 1);
  else if(!strcmp(k, "nightFrom"))  s.nightFrom = clampi(v, 0, 47);
  else if(!strcmp(k, "nightTo"))    s.nightTo = clampi(v, 0, 47);
  else if(!strcmp(k, "nightLevel")) s.nightLevel = clampi(v, 0, 100);
  else if(!strcmp(k, "ledMode"))    s.ledMode = clampi(v, 0, 2);
  else if(!strcmp(k, "batSave"))    s.batSave = clampi(v, 0, 4);
  else if(!strcmp(k, "favHide"))    s.favHide = clampi(v, 0, 1);
  else if(!strcmp(k, "dst"))        { s.dstMode = clampi(v, 0, 1); config.applyTz(); }
  else if(!strcmp(k, "noBat"))      { s.noBat = clampi(v, 0, 1); display.requestRedraw(); }
  else if(!strcmp(k, "noIp"))       { s.noIp = clampi(v, 0, 1); display.requestRedraw(); }
  else if(!strcmp(k, "noSd")){
    s.noSd = clampi(v, 0, 1);
    if(s.noSd){ recorder.stop(); if(config.getMode() == PM_SDCARD) config.changeMode(PM_WEB); }
    display.requestRedraw();
  }
  /*  звук  */
  else if(!strcmp(k, "eqOn"))       { s.eqOn = clampi(v, 0, 1); yoDsp.changed(); }
  else if(!strcmp(k, "eqPreset"))   { yoDsp.applyPreset(clampi(v, 0, EQ_PRESETS - 1)); ext = false; }
  else if(!strcmp(k, "eqBand"))     { int b = atoi(v); const char* c = strchr(v, ':'); if(!c) return; yoDsp.setBand(clampi(v, 0, EQ_BANDS - 1), clampi(c + 1, -12, 12)); (void)b; ext = false; }
  else if(!strcmp(k, "eqLoud"))     { s.eqLoud = clampi(v, 0, 2); yoDsp.changed(); }
  else if(!strcmp(k, "eqGuard"))    { s.eqGuard = clampi(v, 0, 2); yoDsp.changed(); }
  else if(!strcmp(k, "vbass"))      { s.vbass = clampi(v, 0, 3); yoDsp.changed(); }
  else if(!strcmp(k, "eqRoomOn"))   { s.eqRoomOn = clampi(v, 0, 1); yoDsp.changed(); }
  else if(!strcmp(k, "roomTune"))   { if(!s.micOn){ s.micOn = 1; mic.apply(); } const char* w = yoDsp.roomTuneStart(); if(w) _msg = w; }
  else if(!strcmp(k, "roomStop"))   { mic.sweepAbort(); ext = false; }
  else if(!strcmp(k, "roomClear"))  { yoDsp.roomClear(); ext = false; }
  else if(!strcmp(k, "bal"))        { config.setBalance(clampi(v, -16, 16)); ext = false; }
  /*  мікрофон  */
  else if(!strcmp(k, "micOn"))      { s.micOn = clampi(v, 0, 1); mic.apply(); }
  else if(!strcmp(k, "micGain"))    { s.micGain = clampi(v, 0, 8); mic.apply(); }
  else if(!strcmp(k, "micPlay"))    s.micPlay = clampi(v, 0, 1);
  else if(!strcmp(k, "clapOn"))     { s.clapOn = clampi(v, 0, 1); if(s.clapOn && !s.micOn){ s.micOn = 1; mic.apply(); } }
  else if(!strcmp(k, "clapSens"))   s.clapSens = clampi(v, 0, 2);
  else if(!strcmp(k, "clap2"))      s.clap2 = clampi(v, 0, MA_N - 1);
  else if(!strcmp(k, "clap3"))      s.clap3 = clampi(v, 0, MA_N - 1);
  else if(!strcmp(k, "knockOn"))    { s.knockOn = clampi(v, 0, 1); if(s.knockOn && !s.micOn){ s.micOn = 1; mic.apply(); } }
  else if(!strcmp(k, "knockSens"))  s.knockSens = clampi(v, 0, 2);
  else if(!strcmp(k, "knock2"))     s.knock2 = clampi(v, 0, MA_N - 1);
  else if(!strcmp(k, "knock3"))     s.knock3 = clampi(v, 0, MA_N - 1);
  else if(!strcmp(k, "sleepEar"))   { s.sleepEar = clampi(v, 0, 1); if(s.sleepEar && !s.micOn){ s.micOn = 1; mic.apply(); } }
  else if(!strcmp(k, "sleepEarMin")) s.sleepEarMin = clampi(v, 1, 60);
  else if(!strcmp(k, "presWake"))   { s.presWake = clampi(v, 0, 1); if(s.presWake && !s.micOn){ s.micOn = 1; mic.apply(); } }
  else if(!strcmp(k, "presOff"))    { s.presOff = clampi(v, 0, 120); if(s.presOff && !s.micOn){ s.micOn = 1; mic.apply(); } }
  else if(!strcmp(k, "dac")){
    uint8_t d = clampi(v, 0, 3);         /* VS1053 — лише окремою прошивкою */
    if(d != s.dac){ s.dac = d; extras.changed(); extras.applyDac(); }
    ext = false;
  }
  else{
    ext = false;
    if(!strcmp(k, "bright")){ config.store.brightness = clampi(v, 0, 100); config.setBrightness(true); }
    else if(!strcmp(k, "favSet"))   { if(!extras.favSetCurrent(clampi(v, 0, FAV_N - 1))) _msg = "зараз не грає радіостанція"; }
    else if(!strcmp(k, "favClear")) extras.favClear(clampi(v, 0, FAV_N - 1));
    else if(!strcmp(k, "favPlay"))  { if(!extras.favPlay(clampi(v, 0, FAV_N - 1))) _msg = "цієї станції вже немає в списку"; }
    else if(!strcmp(k, "favPut")){
      /*  «клітинка \t назва \t адреса» — обране прямо зі списку на сторінці  */
      char b[240]; strlcpy(b, v, sizeof(b));
      char* t1 = strchr(b, '\t'); char* t2 = t1 ? strchr(t1 + 1, '\t') : nullptr;
      int i = atoi(b);
      if(t1 && t2 && i >= 0 && i < FAV_N){
        *t2 = 0;
        strlcpy(extras.fav[i].name, t1 + 1, sizeof(extras.fav[i].name)); utf8FixTail(extras.fav[i].name);
        strlcpy(extras.fav[i].url, t2 + 1, sizeof(extras.fav[i].url));
        extras.changed();
      }
    }
    else if(!strcmp(k, "sermLoad")) sermons.fetch();
    else if(!strcmp(k, "serm"))     { if(!sermons.play(clampi(v, 0, SERMON_MAX - 1))) _msg = "проповідь недоступна"; }
    else if(!strcmp(k, "sermRel"))  sermons.playRel(clampi(v, -1, 1));
    else if(!strcmp(k, "seek"))     { if(player.remoteStationName) player.burlSeek(clampi(v, 1, 36000)); }
    else if(!strcmp(k, "rec")){
      if(atoi(v)){ if(recorder.start()) _msg = ""; else _msg = recorder.lastError(); }
      else recorder.stop();
    }
    else if(!strcmp(k, "recDel")){
      String f = v;
      if(safeName(f) && sdman.ready && !(recorder.active() && strstr(recorder.fileName(), v))){
        sdman.remove(("/records/" + f).c_str());
        /*  список картки перебудується при переході на неї  */
        sdman.remove(INDEX_SD_PATH); sdman.remove(PLAYLIST_SD_PATH);
      }
    }
    else if(!strcmp(k, "logoForget")) logos.forget();
    else if(!strcmp(k, "logoDel")){
      /*  прибрати логотип станції — плата знайде його в каталозі заново  */
      String f = String(v) + ".565"; char crc[9];
      if(hexName(f, crc)){ dropLogo(crc); logos.bump(); }
    }
    else if(!strcmp(k, "wifiScan")){
      /*  спроби повернутися в мережу спиняємо: інакше пошук не стартує  */
      if(!_scanning){ network.pauseSta(true); WiFi.scanDelete(); WiFi.scanNetworks(true, false); _scanning = true; _scanT0 = millis(); }
    }
    else if(!strcmp(k, "wifiJoin")){
      /*  Як на самому радіо: підключаємось просто зараз, без перезавантаження,
          а результат сторінка бере з /api/wifi (join). Вдалося — мережа
          стає першою в списку; ні — радіо повертається до збережених.  */
      const char* t = strchr(v, '\t');
      if(!t) return;
      if(network.tryState() == TRY_RUN || _wjRun || _wjStartAt){ _msg = "радіо вже підключається"; return; }
      size_t l = t - v; if(l > 32) l = 32;
      memcpy(_wjS, v, l); _wjS[l] = 0;
      strlcpy(_wjP, t + 1, sizeof(_wjP));
      _wjRes = TRY_RUN; _wjIp[0] = 0; _wjT = millis();
      /*  перемикаємо за пів секунди: відповідь сторінці має встигнути піти,
          поки радіо ще в цій мережі  */
      _wjStartAt = millis() + 600;
    }
    else if(!strcmp(k, "wifiForget") || !strcmp(k, "wifiFirst")){
      char ss[WF_MAX][33], pw[WF_MAX][65];
      uint8_t n = readWifi(ss, pw);
      int f = -1; for(uint8_t i = 0; i < n; i++) if(!strcmp(ss[i], v)) f = i;
      if(f < 0) return;
      if(!strcmp(k, "wifiForget")){
        /*  Прибрати можна й останню: після вимкнення радіо лишиться без мережі
            й попросить вибрати її на екрані; сторінка про це попереджає.  */
        for(uint8_t i = f; i + 1 < n; i++){ strcpy(ss[i], ss[i+1]); strcpy(pw[i], pw[i+1]); }
        n--;
      }else{
        char a[33], b[65]; strcpy(a, ss[f]); strcpy(b, pw[f]);
        for(int i = f; i > 0; i--){ strcpy(ss[i], ss[i-1]); strcpy(pw[i], pw[i-1]); }
        strcpy(ss[0], a); strcpy(pw[0], b);
      }
      /*  Запис іде через config: там і копія списку в NVS, і перечитування
          в пам'ять — без перезавантаження.  */
      String out;
      for(uint8_t i = 0; i < n; i++) out += String(ss[i]) + "\t" + String(pw[i]) + "\n";
      config.saveWifiList(out.c_str());
    }
    else if(!strcmp(k, "plSave")){
      if(config.getMode() != PM_WEB || !SPIFFS.exists(TMP_PATH)) return;
      SPIFFS.remove(PLAYLIST_PATH);
      SPIFFS.rename(TMP_PATH, PLAYLIST_PATH);
      netserver.requestOnChange(PLAYLISTSAVED, 0);   /* переіндексує й скаже сторінкам */
    }
    else if(!strcmp(k, "msgClr")) _msg = "";
    else if(!strcmp(k, "power")){
      /*  «вимкнути» зі сторінки: екран показати не встигнемо — просто спимо  */
      if(!strcmp(v, "off")) extras.requestPower(2);
      else if(!strcmp(v, "reboot")) extras.requestPower(1);
    }
  }
  if(ext) extras.changed();
}

void yoWebApiLoop(){
  if(!_q) return;
  if(_wjStartAt && (int32_t)(millis() - _wjStartAt) >= 0){
    _wjStartAt = 0;
    _wjRun = true;
    network.connectTo(_wjS, _wjP);
  }
  if(_wjRun){
    n_Try_e st = network.tryState();
    if(st == TRY_OK){
      /*  вдалося: мережа — першою в списку, решта збережених за нею  */
      char ss[WF_MAX][33], pw[WF_MAX][65];
      uint8_t n = readWifi(ss, pw);
      String out = String(_wjS) + "\t" + String(_wjP) + "\n";
      uint8_t m = 1;
      for(uint8_t i = 0; i < n && m < WF_MAX; i++){
        if(!strcmp(ss[i], _wjS)) continue;
        out += String(ss[i]) + "\t" + String(pw[i]) + "\n"; m++;
      }
      config.saveWifiList(out.c_str());
      config.setLastSSID(1);
      strlcpy(_wjIp, WiFi.localIP().toString().c_str(), sizeof(_wjIp));
      _wjRes = TRY_OK; _wjRun = false; _wjP[0] = 0;
      network.tryClear();
    }else if(st == TRY_BADPASS || st == TRY_NOTFOUND || st == TRY_FAIL){
      _wjRes = st; _wjRun = false; _wjP[0] = 0;
      network.tryClear();                      /* назад до збережених мереж */
    }else if(st == TRY_NONE){
      _wjRes = TRY_FAIL; _wjRun = false; _wjP[0] = 0;   /* спробу закрило меню радіо */
    }
  }
  WebCmd c;
  /*  по одній за прохід: команда плеєра може тривати, звук важливіший  */
  if(xQueueReceive(_q, &c, 0) == pdTRUE) apply(c);
  if(_recReq){ recList(); _recReq = false; }
  if(_scanning){
    int16_t n = WiFi.scanComplete();
    if(n >= 0 || millis() - _scanT0 > 15000){
      uint8_t m = 0;
      for(int16_t i = 0; i < n && m < SCAN_N; i++){
        String ss = WiFi.SSID(i);
        if(!ss.length()) continue;
        bool dup = false;
        for(uint8_t j = 0; j < m; j++) if(!strcmp(_scan[j].s, ss.c_str())){ dup = true; if(WiFi.RSSI(i) > _scan[j].r) _scan[j].r = WiFi.RSSI(i); }
        if(dup) continue;
        strlcpy(_scan[m].s, ss.c_str(), sizeof(_scan[m].s));
        _scan[m].r = WiFi.RSSI(i);
        _scan[m].e = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        m++;
      }
      /*  найсильніші — першими  */
      for(uint8_t a = 0; a < m; a++) for(uint8_t b = a + 1; b < m; b++)
        if(_scan[b].r > _scan[a].r){ ScanItem t = _scan[a]; _scan[a] = _scan[b]; _scan[b] = t; }
      _scanN = m;
      WiFi.scanDelete();
      _scanning = false;
      network.pauseSta(false);
    }
  }
}
