#include "yoOta.h"
#include "yoExtras.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <SPIFFS.h>
#include "esp_heap_caps.h"
#include "yoVersion.h"
#include "../core/options.h"
#include "../core/network.h"
#include "../core/player.h"

YoOta ota;

/*  Репозиторій — свій для кожної плати (PR_REPO у myoptions.h): ES3C35P має окремий.  */
#ifndef PR_REPO
  #define PR_REPO "roman885-85/potuzhne-radio"
#endif
static const char* const REPO = PR_REPO;
/*  Прошивка — своя для кожної плати (PR_BOARD у myoptions.h). Сторінка радіо
    однакова для обох, тож її файли лишились під старою назвою.  */
static const char* const A_FW  = "PotuzhneRadio-" PR_BOARD "-update.bin";
static const char* const A_JS  = "PotuzhneRadio-ES3C28P-app.js.gz";
static const char* const A_CSS = "PotuzhneRadio-ES3C28P-app.css.gz";

/*  «v1.4.0», «1.4», «1.4.0-beta.2»: числа по черзі; за рівних чисел випуск новіший за бету  */
int YoOta::cmpVersion(const char* a, const char* b){
  if(*a == 'v' || *a == 'V') a++;
  if(*b == 'v' || *b == 'V') b++;
  for(uint8_t i = 0; i < 4; i++){
    long x = strtol(a, (char**)&a, 10), y = strtol(b, (char**)&b, 10);
    if(x != y) return x < y ? -1 : 1;
    bool da = *a == '.', db = *b == '.';
    if(da) a++;
    if(db) b++;
    if(!da && !db) break;
  }
  bool pa = *a == '-', pb = *b == '-';
  if(pa != pb) return pa ? -1 : 1;
  if(pa){
    const char* na = a; while(*na && !isdigit((uint8_t)*na)) na++;
    const char* nb = b; while(*nb && !isdigit((uint8_t)*nb)) nb++;
    long x = atol(na), y = atol(nb);
    if(x != y) return x < y ? -1 : 1;
  }
  return 0;
}

const char* YoOta::stepName() const {
  switch(_st){
    case OTA_CHECKING:  return "перевіряю GitHub";
    case OTA_PREPARE:   return "зупиняю звук";
    case OTA_WEB:       return "завантажую сторінку радіо";
    case OTA_FIRMWARE:  return "завантажую й записую прошивку";
    case OTA_VERIFY:    return "перевіряю прошивку";
    case OTA_DONE:      return "готово, перезавантажуюсь";
    case OTA_ERROR:     return "не вийшло";
    default:            return "";
  }
}

void YoOta::_fail(const char* msg){
  strlcpy(_err, msg, sizeof(_err));
  Serial.printf("##OTA#\tпомилка: %s\n", msg);
  _st = OTA_ERROR;
}

void YoOta::loop(){
  if(busy()) return;
  if(network.status != CONNECTED) return;
  uint32_t now = millis();
  if(now >= _nextCheck){
    _nextCheck = now + 12UL * 3600UL * 1000UL;       /* двічі на добу */
    check(extras.s.otaBeta);
  }
}

void YoOta::check(bool beta){
  if(busy()) return;
  if(network.status != CONNECTED){ _fail("немає мережі"); return; }
  _beta = beta;
  _err[0] = 0;
  _st = OTA_CHECKING;
  if(xTaskCreatePinnedToCore(_checkTask, "otaChk", 8192, this, 1, nullptr, 1) != pdPASS) _fail("не вистачило пам'яті");
}

void YoOta::install(){
  if(busy() || !_avail || !_urlFw[0]) return;
  _err[0] = 0;
  _st = OTA_PREPARE; _pct = 0; _got = 0; _size = 0; _bps = 0;
  if(xTaskCreatePinnedToCore(_installTask, "otaIns", 10240, this, 1, nullptr, 1) != pdPASS) _fail("не вистачило пам'яті");
}

void YoOta::_checkTask(void* arg){
  YoOta* o = (YoOta*)arg;
  if(o->_doCheck()){
    o->_st = o->_avail ? OTA_AVAILABLE : OTA_LATEST;
  }
  o->_checkedMs = millis();
  vTaskDelete(nullptr);
}

/*  рядок JSON з позиції p (після лапки) → out, з розбором \n, \", \uXXXX  */
static const char* jsonStr(const char* p, const char* end, char* out, size_t cap){
  size_t n = 0;
  while(p < end && *p && *p != '"'){
    uint32_t c = (uint8_t)*p++;
    if(c >= 0x80){
      /*  GitHub віддає кирилицю як є, байтами UTF-8, — переносимо без змін
          (раніше кожен байт кодувався наново, і в пропозиції стояли «?????»)  */
      if(n + 1 >= cap) break;
      out[n++] = (char)c;
      continue;
    }
    if(c == '\\' && p < end){
      char e = *p++;
      if(e == 'n') c = '\n'; else if(e == 'r') c = 0; else if(e == 't') c = ' ';
      else if(e == 'u' && p + 4 <= end){
        char h[5] = { p[0], p[1], p[2], p[3], 0 }; p += 4;
        c = strtoul(h, nullptr, 16);
        if(c >= 0xD800 && c <= 0xDBFF && p + 6 <= end && p[0] == '\\' && p[1] == 'u'){
          char h2[5] = { p[2], p[3], p[4], p[5], 0 }; p += 6;
          uint32_t lo = strtoul(h2, nullptr, 16);
          c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
        }
      }else c = (uint8_t)e;
    }
    if(!c) continue;
    uint8_t b[4]; uint8_t len;
    if(c < 0x80){ b[0] = c; len = 1; }
    else if(c < 0x800){ b[0] = 0xC0 | (c >> 6); b[1] = 0x80 | (c & 0x3F); len = 2; }
    else if(c < 0x10000){ b[0] = 0xE0 | (c >> 12); b[1] = 0x80 | ((c >> 6) & 0x3F); b[2] = 0x80 | (c & 0x3F); len = 3; }
    else { b[0] = 0xF0 | (c >> 18); b[1] = 0x80 | ((c >> 12) & 0x3F); b[2] = 0x80 | ((c >> 6) & 0x3F); b[3] = 0x80 | (c & 0x3F); len = 4; }
    if(n + len >= cap) break;
    memcpy(out + n, b, len); n += len;
  }
  out[n] = 0;
  return p;
}

static const char* findIn(const char* from, const char* end, const char* needle){
  size_t nl = strlen(needle);
  for(const char* p = from; p + nl <= end; p++) if(!memcmp(p, needle, nl)) return p;
  return nullptr;
}

bool YoOta::_doCheck(){
  const size_t CAP = 160 * 1024;
  char* buf = (char*)heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
  if(!buf){ _fail("не вистачило пам'яті"); return false; }
  size_t len = 0;
  {
    WiFiClientSecure cli; cli.setInsecure();
    HTTPClient http;
    http.useHTTP10(true);                         /* без відповіді частинами (chunked): потік читаємо сирим */
    http.setTimeout(8000); http.setConnectTimeout(8000);
    char url[160];
    if(_beta) snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases?per_page=3", REPO);
    else snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest", REPO);
    if(!http.begin(cli, url)){ free(buf); _fail("не вдалося звернутися до GitHub"); return false; }
    http.addHeader("User-Agent", "potuzhne-radio");
    http.addHeader("Accept", "application/vnd.github+json");
    int code = http.GET();
    if(code != 200){
      http.end(); free(buf);
      char m[64]; snprintf(m, sizeof(m), code == 404 ? "на GitHub ще немає випуску" : "GitHub відповів %d", code);
      _fail(m); return false;
    }
    WiFiClient* st = http.getStreamPtr();
    uint32_t deadline = millis() + 15000;
    int total = http.getSize();
    while(len < CAP - 1 && (int32_t)(deadline - millis()) > 0 && (total < 0 || (int)len < total)){
      int av = st->available();
      if(!av){ if(!st->connected()) break; vTaskDelay(pdMS_TO_TICKS(5)); continue; }
      len += st->read((uint8_t*)buf + len, (size_t)av > CAP - 1 - len ? CAP - 1 - len : av);
    }
    http.end();
  }
  buf[len] = 0;
  const char* end = buf + len;
  /*  перший випуск у відповіді (у списку — найсвіжіший), до наступного tag_name  */
  const char* t = findIn(buf, end, "\"tag_name\":\"");
  if(!t){ free(buf); _fail("незрозуміла відповідь GitHub"); return false; }
  const char* next = findIn(t + 12, end, "\"tag_name\":\"");
  const char* rend = next ? next : end;
  char tag[32]; jsonStr(t + 12, rend, tag, sizeof(tag));
  char fw[240] = { 0 }, js[240] = { 0 }, css[240] = { 0 };
  uint32_t fwSize = 0;
  for(const char* p = buf; (p = findIn(p, rend, "\"browser_download_url\":\"")); ){
    char u[240]; const char* q = jsonStr(p + 24, rend, u, sizeof(u));
    const char* slash = strrchr(u, '/');
    const char* name = slash ? slash + 1 : u;
    if(!strcmp(name, A_FW)){
      strlcpy(fw, u, sizeof(fw));
      /*  розмір — поле "size" цього ж файлу, воно стоїть перед адресою  */
      const char* s = nullptr;
      for(const char* k = p; k > buf + 7; k--) if(!memcmp(k - 7, "\"size\":", 7)){ s = k; break; }
      if(s) fwSize = strtoul(s, nullptr, 10);
    }
    else if(!strcmp(name, A_JS)) strlcpy(js, u, sizeof(js));
    else if(!strcmp(name, A_CSS)) strlcpy(css, u, sizeof(css));
    p = q;
  }
  char notes[640] = { 0 };
  const char* b = findIn(t, rend, "\"body\":\"");
  if(b) jsonStr(b + 8, rend, notes, sizeof(notes));
  free(buf);
  strlcpy(_tag, tag, sizeof(_tag));
  strlcpy(_notes, notes, sizeof(_notes));
  strlcpy(_urlFw, fw, sizeof(_urlFw));
  strlcpy(_urlJs, js, sizeof(_urlJs));
  strlcpy(_urlCss, css, sizeof(_urlCss));
  _sizeFw = fwSize;
  _avail = fw[0] && cmpVersion(tag, prVersion()) > 0;
  Serial.printf("##OTA#\tна GitHub %s (у радіо %s): %s\n", tag, prVersion(), _avail ? "є новіша" : (fw[0] ? "новішої немає" : "без файлу прошивки"));
  return true;
}

void YoOta::_installTask(void* arg){
  YoOta* o = (YoOta*)arg;
  if(o->_doInstall()){
    o->_st = OTA_DONE;
    Serial.println("##OTA#\tготово, перезавантаження");
    vTaskDelay(pdMS_TO_TICKS(2500));
    ESP.restart();
  }
  vTaskDelete(nullptr);
}

/*  Завантажити файл у пам'ять (сторінка радіо — сотні кілобайт).  */
static uint8_t* fetchSmall(const char* url, size_t& len, char* err, size_t errCap){
  len = 0;
  const size_t CAP = 600 * 1024;
  uint8_t* buf = (uint8_t*)heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
  if(!buf){ strlcpy(err, "не вистачило пам'яті", errCap); return nullptr; }
  WiFiClientSecure cli; cli.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);                         /* без відповіді частинами (chunked): потік читаємо сирим */
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(10000); http.setConnectTimeout(10000);
  if(!http.begin(cli, url)){ free(buf); strlcpy(err, "не вдалося звернутися до GitHub", errCap); return nullptr; }
  http.addHeader("User-Agent", "potuzhne-radio");
  int code = http.GET();
  if(code != 200){ http.end(); free(buf); snprintf(err, errCap, "файл сторінки: код %d", code); return nullptr; }
  int total = http.getSize();
  WiFiClient* st = http.getStreamPtr();
  uint32_t deadline = millis() + 30000;
  while(len < CAP && (int32_t)(deadline - millis()) > 0 && (total < 0 || (int)len < total)){
    int av = st->available();
    if(!av){ if(!st->connected()) break; vTaskDelay(pdMS_TO_TICKS(5)); continue; }
    len += st->read(buf + len, (size_t)av > CAP - len ? CAP - len : av);
  }
  http.end();
  if(total > 0 && (int)len != total){ free(buf); strlcpy(err, "файл сторінки обірвався", errCap); return nullptr; }
  return buf;
}

bool YoOta::_doInstall(){
  /*  1. тиша: і процесор, і мережа — для завантаження. lockOutput=true, щоб
      зупинка не скидала «автостарт»: якщо радіо грало до оновлення, після
      перезавантаження воно має заграти саме (інакше після кожного апдейта тиша).  */
  Serial.printf("##OTA#\tвстановлюю %s\n", _tag);
  player.lockOutput = true;
  player.sendCommand({PR_STOP, 0});
  vTaskDelay(pdMS_TO_TICKS(900));

  /*  2. файли сторінки — у пам'ять (запишемо, лише коли прошивка вдасться)  */
  uint8_t* js = nullptr, *css = nullptr; size_t jsLen = 0, cssLen = 0;
  if(_urlJs[0] || _urlCss[0]){
    _st = OTA_WEB; _pct = 0;
    if(_urlJs[0]){ js = fetchSmall(_urlJs, jsLen, _err, sizeof(_err)); if(!js){ _fail(_err); return false; } }
    _pct = 50;
    if(_urlCss[0]){ css = fetchSmall(_urlCss, cssLen, _err, sizeof(_err)); if(!css){ if(js) free(js); _fail(_err); return false; } }
    _pct = 100;
  }

  /*  3. прошивка — одразу в другий розділ  */
  _st = OTA_FIRMWARE; _pct = 0; _got = 0;
  uint8_t* chunk = (uint8_t*)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
  bool ok = false;
  if(chunk){
    WiFiClientSecure cli; cli.setInsecure();
    HTTPClient http;
    http.useHTTP10(true);                         /* без відповіді частинами (chunked): потік читаємо сирим */
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(15000); http.setConnectTimeout(10000);
    if(!http.begin(cli, _urlFw)) _fail("не вдалося звернутися до GitHub");
    else{
      http.addHeader("User-Agent", "potuzhne-radio");
      int code = http.GET();
      int total = http.getSize();
      if(code != 200){ char m[48]; snprintf(m, sizeof(m), "прошивка: код %d", code); _fail(m); }
      else if(total <= 0){ _fail("GitHub не назвав розмір прошивки"); }
      else if(!Update.begin(total, U_FLASH)){ _fail("немає місця для нової прошивки"); }
      else{
        _size = total;
        WiFiClient* st = http.getStreamPtr();
        uint32_t last = millis(), t0 = millis();
        while(_got < (uint32_t)total){
          int av = st->available();
          if(!av){
            if(!st->connected() || millis() - last > 20000) break;
            vTaskDelay(pdMS_TO_TICKS(5)); continue;
          }
          int n = st->read(chunk, av > 8192 ? 8192 : av);
          if(n <= 0) continue;
          if(Update.write(chunk, n) != (size_t)n){ _fail("не вдалося записати прошивку"); break; }
          _got += n; last = millis();
          _pct = (uint8_t)((uint64_t)_got * 100 / total);
          uint32_t el = millis() - t0;
          if(el > 500) _bps = (uint32_t)((uint64_t)_got * 1000 / el);
        }
        if(_st == OTA_FIRMWARE){
          if(_got != (uint32_t)total){ Update.abort(); _fail("завантаження обірвалось — спробуйте ще раз"); }
          else{
            _st = OTA_VERIFY;
            if(!Update.end(true)) _fail("прошивка не пройшла перевірку");
            else ok = true;
          }
        }else Update.abort();
      }
      http.end();
    }
    free(chunk);
  }else _fail("не вистачило пам'яті");

  /*  4. сторінка радіо — тепер, коли нова прошивка вже стоїть  */
  if(ok){
    auto put = [](const char* path, const uint8_t* d, size_t n){
      if(!d || !n) return;
      char tmp[40]; snprintf(tmp, sizeof(tmp), "%s.new", path);
      File f = SPIFFS.open(tmp, "w");
      if(!f) return;
      size_t w = f.write(d, n);
      f.close();
      if(w == n){ SPIFFS.remove(path); SPIFFS.rename(tmp, path); }
      else SPIFFS.remove(tmp);
    };
    put("/www/app.js.gz", js, jsLen);
    put("/www/app.css.gz", css, cssLen);
  }
  if(js) free(js);
  if(css) free(css);
  return ok;
}
