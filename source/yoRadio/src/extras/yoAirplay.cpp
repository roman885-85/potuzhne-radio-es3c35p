#include "yoAirplay.h"
#include "../core/options.h"
#include "../core/config.h"
#include "../core/player.h"
#include "../core/network.h"
#include "yoExtras.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <mdns.h>
#include <sys/time.h>
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/aes.h"
#include "mbedtls/sha256.h"
#include "lwip/sockets.h"
#include "lwip/udp.h"
#include "lwip/tcpip.h"
#include "alac/alac.h"

YoAirplay airplay;
const char* friendlyOf();              /* extras/yoDlna.cpp: «ПОТУЖНЕ РАДІО (ім'я)» */

#define RTSP_PORT   5000
#define AUDIO_PORT  6000
#define CTRL_PORT   6001
#define TIME_PORT   6002
#define FPP         352                /* відліків у пакеті — так шлють усі пристрої Apple */
#define NSLOT       512                /* буфер звуку: 512 пакетів ≈ 4 с (ділить 2^16 — номери пакетів ходять по колу) */
#define TARGET      48                 /* скільки набрати перед стартом: ≈0,4 с на нерівний Wi-Fi */
#define RAW_N       64                 /* черга сирих пакетів із мережі до задачі */
#define RAW_MAX     1600
#define REQ_MAX     16384
#define OUT_MAX     2048
#define NCLI        3                  /* одночасних з'єднань RTSP: сесія й перевірки «ти тут?» */
#define KEY_MAX     2048

/*  Звідки взяти ключ AirPort Express: відкритий приймач shairport-sync,
    закріплений коміт (файл не зміниться), і що саме маємо отримати.  */
static const char KEY_URL[] = "https://raw.githubusercontent.com/mikebrady/shairport-sync/7bad231c18368dbd26f298577f6210e36e4b0797/common.c";
static const char KEY_SHA[] = "7cec0bf8683f595f534f6cf2636b162aec7f006324eccdf17669bbc3583f82db";
static const char PEM_BEGIN[] = "-----BEGIN RSA PRIVATE KEY-----";
static const char PEM_END[]   = "-----END RSA PRIVATE KEY-----";
static char* keyPem = nullptr;         /* текст ключа з кінцевим нулем */
static size_t keyLen = 0;

/*  ---------- сирі пакети: мережа → задача ----------
    Звичайний UDP-сокет тримає в черзі лише 6 пакетів (так зібрано lwIP у
    ядрі), а Wi-Fi віддає звук пачками по 10–20: решта губилась би. Тому
    пакети забираємо просто в мережевому стеку й кладемо в свою чергу.  */
struct RawPkt { uint16_t len; uint8_t kind; uint8_t data[RAW_MAX]; };   /* kind: 0 звук, 1 керування */
static RawPkt* raw = nullptr;
static volatile uint16_t rawHead = 0, rawTail = 0;
static volatile uint32_t stRawDrop = 0;
static udp_pcb* pcbA = nullptr;
static udp_pcb* pcbC = nullptr;
static udp_pcb* pcbT = nullptr;
static TaskHandle_t apTask = nullptr;
static volatile bool stopReq = false;

/*  ---------- буфер звуку: задача → головний цикл ---------- */
struct Slot { uint16_t seq; uint16_t frames; uint8_t ready; };
static Slot* slot = nullptr;
static int16_t* slotPcm = nullptr;
static SemaphoreHandle_t mtx = nullptr;
static bool synced = false, filling = true;
static uint16_t rd = 0, wr = 0;
static uint32_t avgFill = 0;
static uint8_t stuffCnt = 0;
static uint32_t stPk = 0, stLost = 0, stUnder = 0, stSkip = 0, stAdd = 0, stDrop = 0, stResend = 0, stLate = 0, stBad = 0;

/*  ---------- сесія ---------- */
struct Cli { int fd; char* buf; size_t len; size_t discard; };
static Cli cli[NCLI] = { { -1, nullptr, 0, 0 }, { -1, nullptr, 0, 0 }, { -1, nullptr, 0, 0 } };
static int lsock = -1;
static int owner = -1;                 /* чиє з'єднання веде потік */
static int cur = -1;                   /* на яке з'єднання зараз відповідаємо */
static char* outBuf = nullptr;
static char* exBuf = nullptr;
static uint8_t* decBuf = nullptr;
static int16_t* tmpPcm = nullptr;
static bool enc = false, isAlac = true;
static uint8_t aesIv[16];
static mbedtls_aes_context aes;
static bool aesInit = false;
static alac_file* alacDec = nullptr;
static ip_addr_t clientIp;
static uint16_t cport = 0, tport = 0;
static uint32_t lastTiming = 0;
static uint8_t timingN = 0;
static float volDb = -15.0f;
static uint32_t outReqAt = 0;

/*  ---------- дрібниці ---------- */

static int b64val(char c){
  if(c >= 'A' && c <= 'Z') return c - 'A';
  if(c >= 'a' && c <= 'z') return c - 'a' + 26;
  if(c >= '0' && c <= '9') return c - '0' + 52;
  if(c == '+') return 62;
  if(c == '/') return 63;
  return -1;
}
/*  Apple шле base64 без «=» у кінці — бібліотечний розбір такого не приймає.  */
static size_t b64dec(const char* s, uint8_t* out, size_t max){
  uint32_t acc = 0; int bits = 0; size_t n = 0;
  for(; *s; s++){
    if(*s == '=' || *s == ' ' || *s == '\t') continue;
    int v = b64val(*s);
    if(v < 0) break;
    acc = (acc << 6) | (uint32_t)v; bits += 6;
    if(bits >= 8){ bits -= 8; if(n < max) out[n++] = (uint8_t)(acc >> bits); }
  }
  return n;
}
static void b64enc(const uint8_t* in, size_t n, char* out, size_t max){
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  uint32_t acc = 0; int bits = 0; size_t o = 0;
  for(size_t i = 0; i < n; i++){
    acc = (acc << 8) | in[i]; bits += 8;
    while(bits >= 6){ bits -= 6; if(o + 1 < max) out[o++] = T[(acc >> bits) & 63]; }
  }
  if(bits > 0 && o + 1 < max) out[o++] = T[(acc << (6 - bits)) & 63];
  out[o] = 0;
}

static int rng(void*, unsigned char* b, size_t n){ esp_fill_random(b, n); return 0; }

/*  Розібраний ключ тримаємо готовим: розбір 2048-бітного ключа з PEM коштує
    сотні мілісекунд, а раніше він робився на КОЖЕН запит RTSP (двічі за
    рукопожаття). Під навантаженням (грає станція) це виходило за таймаут
    пристрою Apple — AirPlay не запускався. Тепер розбираємо один раз.  */
static mbedtls_pk_context s_pk;
static volatile bool s_pkOk = false;
static bool ensurePk(){
  if(s_pkOk) return true;
  if(!keyPem || !keyLen) return false;
  mbedtls_pk_init(&s_pk);
  int r = mbedtls_pk_parse_key(&s_pk, (const unsigned char*)keyPem, keyLen + 1, nullptr, 0, rng, nullptr);
  if(r == 0 && mbedtls_pk_get_type(&s_pk) == MBEDTLS_PK_RSA){ s_pkOk = true; return true; }
  Serial.printf("##AIRPLAY#\tRSA: ключ не розібрався -0x%04X\n", (unsigned)-r);
  mbedtls_pk_free(&s_pk);
  return false;
}

/*  sign: підписати виклик (PKCS#1 v1.5); інакше — розшифрувати ключ AES (OAEP, SHA-1).
    Кличеться лише із задачі airplay (RTSP), тож спільний s_pk без блокувань.  */
static int rsaApply(bool sign, const uint8_t* in, size_t inLen, uint8_t* out, size_t outMax){
  if(!ensurePk()) return -1;
  mbedtls_rsa_context* rsa = mbedtls_pk_rsa(s_pk);
  int len = -1, r = 0;
  if(sign){
    mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE);
    const size_t kl = mbedtls_rsa_get_len(rsa);
    if(kl <= outMax){
      r = mbedtls_rsa_pkcs1_sign(rsa, rng, nullptr, MBEDTLS_MD_NONE, (unsigned)inLen, in, out);
      if(r == 0) len = (int)kl;
    }
  }else{
    mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
    size_t ol = 0;
    r = mbedtls_rsa_pkcs1_decrypt(rsa, rng, nullptr, &ol, in, out, outMax);
    if(r == 0) len = (int)ol;
  }
  if(r) Serial.printf("##AIRPLAY#\tRSA: помилка -0x%04X\n", (unsigned)-r);
  return len;
}

static void be32put(uint8_t* o, uint32_t v){ o[0] = v >> 24; o[1] = v >> 16; o[2] = v >> 8; o[3] = v; }
static uint32_t be32(const uint8_t* p){ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static void ntpNow(uint8_t* o){
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  be32put(o, (uint32_t)tv.tv_sec + 2208988800UL);
  be32put(o + 4, (uint32_t)(((uint64_t)tv.tv_usec << 32) / 1000000ULL));
}

/*  Значення заголовка RTSP (без урахування регістру назви).  */
static bool hget(const char* h, size_t hl, const char* name, char* out, size_t n){
  const size_t nl = strlen(name);
  const char* p = h;
  const char* end = h + hl;
  while(p < end){
    const char* eol = (const char*)memchr(p, '\n', end - p);
    if(!eol) eol = end;
    if((size_t)(eol - p) > nl && !strncasecmp(p, name, nl) && p[nl] == ':'){
      const char* v = p + nl + 1;
      while(v < eol && *v == ' ') v++;
      size_t L = eol - v;
      if(L && v[L - 1] == '\r') L--;
      if(L >= n) L = n - 1;
      memcpy(out, v, L); out[L] = 0;
      return true;
    }
    p = eol + 1;
  }
  return false;
}

static void sendAll(int fd, const char* d, size_t n){
  uint32_t t0 = millis();
  while(n && millis() - t0 < 2000){
    int w = send(fd, d, n, 0);
    if(w > 0){ d += w; n -= w; continue; }
    if(w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)){ vTaskDelay(pdMS_TO_TICKS(5)); continue; }
    break;
  }
}

static void reply(int fd, const char* status, const char* cseq, const char* extra, const char* body = nullptr){
  const size_t bl = body ? strlen(body) : 0;
  char cl[40] = {0};
  if(bl) snprintf(cl, sizeof(cl), "Content-Length: %u\r\n", (unsigned)bl);
  int n = snprintf(outBuf, OUT_MAX,
    "RTSP/1.0 %s\r\nCSeq: %s\r\nServer: AirTunes/105.1\r\nAudio-Jack-Status: connected; type=analog\r\n%s%s\r\n",
    status, cseq, extra ? extra : "", cl);
  if(n > 0) sendAll(fd, outBuf, (size_t)n < OUT_MAX ? (size_t)n : OUT_MAX - 1);
  if(bl) sendAll(fd, body, bl);
}

/*  ---------- ключ ---------- */

static void sha256hex(const uint8_t* d, size_t n, char* out){
  uint8_t h[32];
  mbedtls_sha256(d, n, h, 0);
  for(uint8_t i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", h[i]);
  out[64] = 0;
}

/*  З тексту C-файла: від рядка з BEGIN до END склеюємо рядкові літерали
    ("...\n"), розгортаючи \n. Результат — звичайний PEM.  */
static size_t pemFromSource(const char* s, size_t n, char* out, size_t max){
  const char* b = (const char*)memmem(s, n, PEM_BEGIN, sizeof(PEM_BEGIN) - 1);
  if(!b) return 0;
  const char* p = b;
  while(p > s && *(p - 1) != '"') p--;
  const char* end = s + n;
  size_t o = 0;
  bool inStr = true;
  for(; p < end && o + 2 < max; p++){
    if(inStr){
      if(*p == '"'){ inStr = false; continue; }
      if(*p == '\\' && p + 1 < end){
        p++;
        if(*p == 'n') out[o++] = '\n';
        else if(*p == '0') { /* кінець рядка C */ }
        else out[o++] = *p;
        continue;
      }
      out[o++] = *p;
      out[o] = 0;
      if(o >= sizeof(PEM_END) - 1 && !memcmp(out + o - (sizeof(PEM_END) - 1), PEM_END, sizeof(PEM_END) - 1)){
        out[o++] = '\n'; out[o] = 0;
        return o;
      }
    }else if(*p == '"') inStr = true;
  }
  return 0;
}

bool YoAirplay::_loadKey(){
  if(_keyOk) return true;
  if(!keyPem) keyPem = (char*)heap_caps_calloc(1, KEY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if(!keyPem) return false;
  Preferences p;
  if(!p.begin("yoAirplay", true)) return false;
  size_t n = p.getBytesLength("key");
  if(n > 0 && n < KEY_MAX){
    p.getBytes("key", keyPem, n);
    keyPem[n] = 0;
    char h[65];
    sha256hex((const uint8_t*)keyPem, n, h);
    if(!strcmp(h, KEY_SHA)){ keyLen = n; _keyOk = true; }
  }
  p.end();
  return _keyOk;
}

void YoAirplay::_keyTask(void* arg){
  YoAirplay* self = (YoAirplay*)arg;
  const size_t CAP = 120 * 1024;
  char* buf = (char*)heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  size_t len = 0;
  int code = 0;
  if(buf){
    WiFiClientSecure cli; cli.setInsecure();         /* що отримали — перевіряємо за SHA-256 нижче */
    HTTPClient http;
    http.useHTTP10(true);                            /* без відповіді частинами (chunked): потік читаємо сирим */
    http.setTimeout(10000); http.setConnectTimeout(10000);
    if(http.begin(cli, KEY_URL)){
      http.addHeader("User-Agent", "potuzhne-radio");
      code = http.GET();
      if(code == 200){
        WiFiClient* st = http.getStreamPtr();
        uint32_t deadline = millis() + 30000;
        int total = http.getSize();
        while(len < CAP - 1 && (int32_t)(deadline - millis()) > 0 && (total < 0 || (int)len < total)){
          int av = st->available();
          if(!av){ if(!st->connected()) break; vTaskDelay(pdMS_TO_TICKS(5)); continue; }
          len += st->read((uint8_t*)buf + len, (size_t)av > CAP - 1 - len ? CAP - 1 - len : av);
        }
      }
      http.end();
    }
  }
  bool ok = false;
  if(buf && len){
    char* pem = (char*)heap_caps_calloc(1, KEY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t n = pem ? pemFromSource(buf, len, pem, KEY_MAX) : 0;
    char h[65] = {0};
    if(n) sha256hex((const uint8_t*)pem, n, h);
    if(n && !strcmp(h, KEY_SHA)){
      Preferences p;
      if(p.begin("yoAirplay", false)){ p.putBytes("key", pem, n); p.end(); }
      if(!keyPem) keyPem = (char*)heap_caps_calloc(1, KEY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if(keyPem){ memcpy(keyPem, pem, n); keyPem[n] = 0; keyLen = n; s_pkOk = false; ok = true; }
    }else{
      snprintf(self->_keyErr, sizeof(self->_keyErr), n ? "ключ не збігся (SHA-256)" : "ключа у файлі не знайдено");
    }
    free(pem);
  }else{
    snprintf(self->_keyErr, sizeof(self->_keyErr), buf ? "GitHub відповів %d" : "не вистачило пам'яті", code);
  }
  free(buf);
  if(ok){ self->_keyErr[0] = 0; self->_keyOk = true; Serial.println("##AIRPLAY#\tключ AirPort отримано й перевірено"); }
  else Serial.printf("##AIRPLAY#\tключ не отримано: %s\n", self->_keyErr);
  self->_keyNext = millis() + (ok ? 0 : 10UL * 60UL * 1000UL);   /* не вийшло — ще раз за 10 хв */
  self->_keyBusy = false;
  vTaskDelete(nullptr);
}

void YoAirplay::loop(){
  if(!on() || !_task) return;
  if(!_keyOk){
    if(_keyBusy || network.status != CONNECTED || millis() < _keyNext) return;
    _keyBusy = true;
    if(xTaskCreatePinnedToCore(_keyTask, "apKey", 8192, this, 1, nullptr, 1) != pdPASS){
      _keyBusy = false; _keyNext = millis() + 60000;
    }
    return;
  }
  if(!_mdnsOn) _mdns();
}

/*  ---------- UDP у мережевому стеку ---------- */

static void udpIn(void* arg, udp_pcb* pcb, pbuf* p, const ip_addr_t* addr, u16_t port){
  if(!p) return;
  const uint8_t kind = (uint8_t)(uintptr_t)arg;
  if(kind == 2){
    /*  Запит часу від телефона — відповідаємо одразу, тут же.  */
    uint8_t q[32];
    if(p->tot_len >= 32 && pbuf_copy_partial(p, q, 32, 0) == 32 && (q[1] & 0x7F) == 0x52){
      pbuf* r = pbuf_alloc(PBUF_TRANSPORT, 32, PBUF_RAM);
      if(r){
        uint8_t* o = (uint8_t*)r->payload;
        memset(o, 0, 32);
        o[0] = 0x80; o[1] = 0xD3; o[2] = q[2]; o[3] = q[3];
        memcpy(o + 8, q + 24, 8);
        ntpNow(o + 16);
        memcpy(o + 24, o + 16, 8);
        udp_sendto(pcb, r, addr, port);
        pbuf_free(r);
      }
    }
    pbuf_free(p);
    return;
  }
  const uint16_t h = rawHead, nh = (uint16_t)((h + 1) % RAW_N);
  if(raw && nh != rawTail && p->tot_len <= RAW_MAX){
    raw[h].len = pbuf_copy_partial(p, raw[h].data, p->tot_len, 0);
    raw[h].kind = kind;
    rawHead = nh;
    if(apTask) xTaskNotifyGive(apTask);
  }else stRawDrop++;
  pbuf_free(p);
}

static udp_pcb* udpOpen(uint16_t port, uint8_t kind){
  LOCK_TCPIP_CORE();
  udp_pcb* pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
  if(pcb){
    if(udp_bind(pcb, IP_ANY_TYPE, port) != ERR_OK){ udp_remove(pcb); pcb = nullptr; }
    else udp_recv(pcb, udpIn, (void*)(uintptr_t)kind);
  }
  UNLOCK_TCPIP_CORE();
  if(!pcb) Serial.printf("##AIRPLAY#\tпорт UDP %u зайнятий\n", (unsigned)port);
  return pcb;
}

static void udpClose(udp_pcb*& pcb){
  if(!pcb) return;
  LOCK_TCPIP_CORE();
  udp_recv(pcb, nullptr, nullptr);
  udp_remove(pcb);
  UNLOCK_TCPIP_CORE();
  pcb = nullptr;
}

static void udpSend(udp_pcb* pcb, const uint8_t* d, uint16_t n, uint16_t port){
  if(!pcb || !port) return;
  LOCK_TCPIP_CORE();
  pbuf* b = pbuf_alloc(PBUF_TRANSPORT, n, PBUF_RAM);
  if(b){
    memcpy(b->payload, d, n);
    udp_sendto(pcb, b, &clientIp, port);
    pbuf_free(b);
  }
  UNLOCK_TCPIP_CORE();
}

/*  ---------- увімкнення ---------- */

bool YoAirplay::on() const { return extras.s.airplayOn != 0; }

void YoAirplay::setOn(bool v){
  if(on() == v) return;
  extras.s.airplayOn = v ? 1 : 0;
  extras.changed();
  if(v) begin(); else stop();
}

bool YoAirplay::_mdns(){
  if(_mdnsOn) return true;
  /*  Без піднятого mDNS до нього не можна торкатись: бібліотека не
      перевіряє власний стан і валить систему прямо в setup().  */
  if(!network.mdnsReady) return false;
  if(mdns_service_exists("_raop", "_tcp", nullptr)){ _mdnsOn = true; return true; }
  uint8_t mac[6];
  WiFi.macAddress(mac);
  /*  Ім'я служби — «MAC@назва»: телефон показує частину після @, а MAC
      має збігатися з тим, що ми підписуємо у відповідь на виклик.  */
  char inst[64];
  int n = snprintf(inst, sizeof(inst), "%02X%02X%02X%02X%02X%02X@%s", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], friendlyOf());
  if(n >= (int)sizeof(inst)){
    size_t L = sizeof(inst) - 1;
    while(L && ((uint8_t)inst[L] & 0xC0) == 0x80) L--;        /* не різати літеру UTF-8 навпіл */
    inst[L] = 0;
  }
  mdns_txt_item_t txt[] = {
    { "txtvers", "1" }, { "ch", "2" }, { "cn", "0,1" }, { "et", "0,1" }, { "ek", "1" },
    { "sv", "false" }, { "da", "true" }, { "sr", "44100" }, { "ss", "16" }, { "pw", "false" },
    { "vn", "65537" }, { "tp", "UDP" }, { "md", "0,2" }, { "vs", "105.1" },
    { "am", "PotuzhneRadio" }, { "sf", "0x4" }, { "fv", "76400.10" },
  };
  esp_err_t e = mdns_service_add(inst, "_raop", "_tcp", RTSP_PORT, txt, sizeof(txt) / sizeof(txt[0]));
  _mdnsOn = (e == ESP_OK);
  if(_mdnsOn) Serial.printf("##AIRPLAY#\tвидно в мережі як «%s»\n", inst);
  else Serial.printf("##AIRPLAY#\tmDNS: помилка %d\n", (int)e);
  return _mdnsOn;
}

void yoAirplayTask(void* arg){ ((YoAirplay*)arg)->_run(); }

static void* psAlloc(size_t n){ return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

void YoAirplay::begin(){
  if(!on() || _task) return;
  if(WiFi.status() != WL_CONNECTED) return;
  if(!raw)     raw     = (RawPkt*)psAlloc(sizeof(RawPkt) * RAW_N);
  if(!slot)    slot    = (Slot*)psAlloc(sizeof(Slot) * NSLOT);
  if(!slotPcm) slotPcm = (int16_t*)psAlloc((size_t)NSLOT * FPP * 4);
  if(!outBuf)  outBuf  = (char*)psAlloc(OUT_MAX);
  if(!exBuf)   exBuf   = (char*)psAlloc(1024);
  if(!decBuf)  decBuf  = (uint8_t*)psAlloc(RAW_MAX + 32);
  if(!tmpPcm)  tmpPcm  = (int16_t*)psAlloc((FPP + 8) * 4);
  for(uint8_t i = 0; i < NCLI; i++) if(!cli[i].buf){ cli[i].buf = (char*)psAlloc(REQ_MAX); cli[i].fd = -1; }
  if(!mtx) mtx = xSemaphoreCreateMutex();
  bool ok = raw && slot && slotPcm && outBuf && exBuf && decBuf && tmpPcm && mtx;
  for(uint8_t i = 0; i < NCLI; i++) ok = ok && cli[i].buf;
  if(!ok){ Serial.println("##AIRPLAY#\tне вистачило пам'яті"); return; }
  if(!aesInit){ mbedtls_aes_init(&aes); aesInit = true; }
  _loadKey();
  if(_keyOk) ensurePk();            /* розібрати ключ заздалегідь — перше підключення не гальмує */
  rawHead = rawTail = 0;
  stopReq = false;
  TaskHandle_t h = nullptr;
  /*  Пріоритет вищий за головний цикл: черга пакетів не має переповнюватись,
      поки там малюється екран чи пишуться налаштування.  */
  if(xTaskCreatePinnedToCore(yoAirplayTask, "airplay", 8192, this, 5, &h, 0) != pdPASS){
    Serial.println("##AIRPLAY#\tзадача не створилась");
    return;
  }
  _task = h;
  apTask = h;
  if(!pcbA) pcbA = udpOpen(AUDIO_PORT, 0);
  if(!pcbC) pcbC = udpOpen(CTRL_PORT, 1);
  if(!pcbT) pcbT = udpOpen(TIME_PORT, 2);
  if(_keyOk) _mdns();
  Serial.printf("##AIRPLAY#\tколонка AirPlay: %s, порт %u, ключ %s\n", friendlyOf(), (unsigned)RTSP_PORT, _keyOk ? "є" : "ще треба отримати");
}

void YoAirplay::stop(){
  if(!_task) return;
  stopReq = true;
  xTaskNotifyGive((TaskHandle_t)_task);
  for(uint8_t i = 0; i < 100 && _task; i++) vTaskDelay(pdMS_TO_TICKS(10));
  udpClose(pcbA); udpClose(pcbC); udpClose(pcbT);
  if(_mdnsOn){ if(network.mdnsReady) mdns_service_remove("_raop", "_tcp"); _mdnsOn = false; }
  Serial.println("##AIRPLAY#\tвимкнено");
}

/*  ---------- задача ---------- */

void YoAirplay::_run(){
  lsock = socket(AF_INET, SOCK_STREAM, 0);
  if(lsock >= 0){
    int one = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(RTSP_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if(bind(lsock, (sockaddr*)&a, sizeof(a)) != 0 || listen(lsock, 3) != 0){
      Serial.printf("##AIRPLAY#\tпорт %u не відкрився (%d)\n", (unsigned)RTSP_PORT, errno);
      close(lsock); lsock = -1;
    }else fcntl(lsock, F_SETFL, fcntl(lsock, F_GETFL, 0) | O_NONBLOCK);
  }
  while(!stopReq){
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
    while(rawTail != rawHead){
      RawPkt& r = raw[rawTail];
      _packet(r.data, r.len, r.kind);
      rawTail = (uint16_t)((rawTail + 1) % RAW_N);
    }
    if(lsock >= 0) _accept();
    _read();
    if(_session){
      _timing();
      /*  Телефон поставив на паузу й мовчить — радіо показує «зупинено»;
          щойно звук піде знову, воно саме перейде на AirPlay.  */
      if(player.extOn && !_idleSent && millis() - _lastPkt > 5000){
        _idleSent = true; _outReq = false;
        player.sendCommand({PR_EXT, 0});
      }
    }
  }
  _end();
  for(uint8_t i = 0; i < NCLI; i++) if(cli[i].fd >= 0){ close(cli[i].fd); cli[i].fd = -1; }
  if(lsock >= 0){ close(lsock); lsock = -1; }
  apTask = nullptr;
  _task = nullptr;
  vTaskDelete(nullptr);
}

void YoAirplay::_accept(){
  sockaddr_in ca;
  socklen_t cl = sizeof(ca);
  int s = accept(lsock, (sockaddr*)&ca, &cl);
  if(s < 0) return;
  int slotI = -1;
  for(uint8_t i = 0; i < NCLI; i++) if(cli[i].fd < 0){ slotI = i; break; }
  if(slotI < 0){
    /*  Усі зайняті: звільняємо перше з тих, що не ведуть потік.  */
    for(uint8_t i = 0; i < NCLI; i++) if(i != owner){ close(cli[i].fd); cli[i].fd = -1; slotI = i; break; }
  }
  fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
  int one = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  cli[slotI].fd = s; cli[slotI].len = 0; cli[slotI].discard = 0; cli[slotI].buf[0] = 0;
  Serial.printf("##AIRPLAY#\tз'єднання з %s\n", inet_ntoa(ca.sin_addr));
}

void YoAirplay::_read(){
  for(uint8_t i = 0; i < NCLI; i++){
    Cli& c = cli[i];
    if(c.fd < 0) continue;
    int n = recv(c.fd, c.buf + c.len, REQ_MAX - 1 - c.len, 0);
    if(n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)){
      close(c.fd); c.fd = -1; c.len = 0;
      if(i == owner){ Serial.println("##AIRPLAY#\tз'єднання закрито"); _end(); }
      continue;
    }
    if(n < 0) continue;
    c.len += n; c.buf[c.len] = 0;
    for(;;){
      if(c.discard){
        const size_t k = c.discard < c.len ? c.discard : c.len;
        memmove(c.buf, c.buf + k, c.len - k); c.len -= k; c.discard -= k; c.buf[c.len] = 0;
        if(c.discard) break;
      }
      char* e = strstr(c.buf, "\r\n\r\n");
      if(!e){ if(c.len >= REQ_MAX - 1) c.len = 0; break; }
      const size_t hl = (e - c.buf) + 4;
      char v[16];
      const size_t bl = hget(c.buf, hl, "Content-Length", v, sizeof(v)) ? (size_t)atol(v) : 0;
      cur = i;
      if(hl + bl > REQ_MAX - 1){
        /*  Надто велике тіло (обкладинка) — відповідаємо без нього, а байти пропускаємо.  */
        _request(c.buf, hl, nullptr, 0);
        if(c.fd < 0) break;
        memmove(c.buf, c.buf + hl, c.len - hl); c.len -= hl; c.buf[c.len] = 0;
        c.discard = bl;
        continue;
      }
      if(c.len < hl + bl) break;
      const char saved = c.buf[hl + bl];
      c.buf[hl + bl] = 0;
      _request(c.buf, hl, c.buf + hl, bl);
      if(c.fd < 0) break;
      c.buf[hl + bl] = saved;
      memmove(c.buf, c.buf + hl + bl, c.len - hl - bl); c.len -= hl + bl; c.buf[c.len] = 0;
    }
  }
}

/*  Назва доріжки з DMAP: minm — назва, asar — виконавець.  */
static void dmap(const uint8_t* p, size_t n, char* title, size_t tn, char* artist, size_t an){
  size_t i = 0;
  while(i + 8 <= n){
    const uint8_t* tag = p + i;
    const uint32_t len = be32(p + i + 4);
    i += 8;
    if(len > n - i) break;
    if(!memcmp(tag, "mlit", 4)) dmap(p + i, len, title, tn, artist, an);
    else if(!memcmp(tag, "minm", 4)){ size_t L = len < tn - 1 ? len : tn - 1; memcpy(title, p + i, L); title[L] = 0; }
    else if(!memcmp(tag, "asar", 4)){ size_t L = len < an - 1 ? len : an - 1; memcpy(artist, p + i, L); artist[L] = 0; }
    i += len;
  }
}

void YoAirplay::_request(char* req, size_t hl, char* body, size_t bl){
  const int fd = cli[cur].fd;
  char method[24] = {0};
  { size_t k = 0; while(k < sizeof(method) - 1 && req[k] && req[k] != ' '){ method[k] = req[k]; k++; } }
  char cseq[16] = "0";
  hget(req, hl, "CSeq", cseq, sizeof(cseq));
  exBuf[0] = 0;
  size_t ex = 0;

  /*  Виклик Apple: підписати «виклик + наша адреса + MAC» ключем AirPort.  */
  char ch[64];
  if(hget(req, hl, "Apple-Challenge", ch, sizeof(ch))){
    uint8_t buf[48] = {0};
    size_t p = b64dec(ch, buf, 16);
    sockaddr_in la; socklen_t ll = sizeof(la);
    if(getsockname(fd, (sockaddr*)&la, &ll) == 0){
      memcpy(buf + p, &la.sin_addr.s_addr, 4); p += 4;
      WiFi.macAddress(buf + p); p += 6;
      if(p < 32) p = 32;
      uint8_t sig[256];
      const int sl = rsaApply(true, buf, p, sig, sizeof(sig));
      if(sl > 0){
        ex += snprintf(exBuf + ex, 1024 - ex, "Apple-Response: ");
        b64enc(sig, sl, exBuf + ex, 1024 - ex);
        ex = strlen(exBuf);
        ex += snprintf(exBuf + ex, 1024 - ex, "\r\n");
      }
    }
  }

  if(!strcmp(method, "OPTIONS")){
    snprintf(exBuf + ex, 1024 - ex, "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER\r\n");
    reply(fd, "200 OK", cseq, exBuf);
  }
  else if(!strcmp(method, "ANNOUNCE")){
    int fmtp[12] = { 96, 352, 0, 16, 40, 10, 14, 2, 255, 0, 0, 44100 };
    bool alac = false, l16 = false, haveKey = false, haveIv = false;
    uint8_t key[16];
    char dev[48] = {0};
    for(char* line = body; line && *line; ){
      char* nl = strchr(line, '\n');
      if(nl) *nl = 0;
      size_t L = strlen(line);
      if(L && line[L - 1] == '\r') line[--L] = 0;
      if(!strncmp(line, "a=rtpmap:", 9)){
        if(strstr(line, "AppleLossless")) alac = true;
        if(strstr(line, "L16/44100/2")) l16 = true;
      }else if(!strncmp(line, "a=fmtp:", 7)){
        char* s = line + 7;
        for(uint8_t k = 0; k < 12 && *s; k++){ fmtp[k] = strtol(s, &s, 10); while(*s == ' ') s++; }
      }else if(!strncmp(line, "a=rsaaeskey:", 12)){
        uint8_t ek[512];
        const size_t n = b64dec(line + 12, ek, sizeof(ek));
        uint8_t kk[256];
        const int kl = n ? rsaApply(false, ek, n, kk, sizeof(kk)) : -1;
        if(kl == 16){ memcpy(key, kk, 16); haveKey = true; }
      }else if(!strncmp(line, "a=aesiv:", 8)){
        haveIv = b64dec(line + 8, aesIv, 16) == 16;
      }else if(!strncmp(line, "i=", 2)){
        strlcpy(dev, line + 2, sizeof(dev));
      }
      line = nl ? nl + 1 : nullptr;
    }
    const bool fmtOk = (alac && fmtp[1] == FPP && fmtp[3] == 16 && fmtp[7] == 2 && fmtp[11] == 44100) || (l16 && !alac);
    if(!fmtOk || haveKey != haveIv){
      Serial.printf("##AIRPLAY#\tформат не підтримується: alac=%d l16=%d кадрів=%d біт=%d канали=%d частота=%d ключ=%d\n",
                    alac, l16, fmtp[1], fmtp[3], fmtp[7], fmtp[11], haveKey);
      reply(fd, "456 Header Field Not Valid for Resource", cseq, exBuf);
      return;
    }
    /*  Новий потік (зокрема з іншого пристрою) забирає колонку собі.  */
    if(_session && owner != cur) _end();
    owner = cur;
    isAlac = alac;
    enc = haveKey;
    if(enc) mbedtls_aes_setkey_dec(&aes, key, 128);
    if(alacDec){ alac_free(alacDec); alacDec = nullptr; }
    if(isAlac){
      alacDec = alac_create(16, 2);
      if(alacDec){
        alacDec->setinfo_max_samples_per_frame = FPP;
        alacDec->setinfo_7a = fmtp[2];
        alacDec->setinfo_sample_size = 16;
        alacDec->setinfo_rice_historymult = fmtp[4];
        alacDec->setinfo_rice_initialhistory = fmtp[5];
        alacDec->setinfo_rice_kmodifier = fmtp[6];
        alacDec->setinfo_7f = fmtp[7];
        alacDec->setinfo_80 = fmtp[8];
        alacDec->setinfo_82 = fmtp[9];
        alacDec->setinfo_86 = fmtp[10];
        alacDec->setinfo_8a_rate = fmtp[11];
        alac_allocate_buffers(alacDec);
      }
    }
    char name[48];
    if(hget(req, hl, "X-Apple-Client-Name", name, sizeof(name)) && name[0]) strlcpy(_device, name, sizeof(_device));
    else if(dev[0] && strcmp(dev, "iTunes") != 0) strlcpy(_device, dev, sizeof(_device));
    else strlcpy(_device, "Пристрій Apple", sizeof(_device));
    _title[0] = 0; _titleNew = false;
    _released = false; _outReq = false; _idleSent = false;
    _reset();
    _session = true;
    _lastPkt = millis();
    Serial.printf("##AIRPLAY#\tпотік від «%s»: %s, %s\n", _device, isAlac ? "ALAC" : "PCM", enc ? "зашифровано" : "відкрито");
    reply(fd, "200 OK", cseq, exBuf);
  }
  else if(!strcmp(method, "SETUP")){
    char tr[256] = {0};
    hget(req, hl, "Transport", tr, sizeof(tr));
    const char* p = strstr(tr, "control_port=");
    cport = p ? (uint16_t)atoi(p + 13) : 0;
    p = strstr(tr, "timing_port=");
    tport = p ? (uint16_t)atoi(p + 12) : 0;
    sockaddr_in pa; socklen_t pl = sizeof(pa);
    if(getpeername(fd, (sockaddr*)&pa, &pl) == 0){
      const uint32_t ip = pa.sin_addr.s_addr;
      IP_ADDR4(&clientIp, ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
    }
    timingN = 0; lastTiming = 0;
    snprintf(exBuf + ex, 1024 - ex,
      "Transport: RTP/AVP/UDP;unicast;mode=record;server_port=%u;control_port=%u;timing_port=%u\r\nSession: 1\r\n",
      (unsigned)AUDIO_PORT, (unsigned)CTRL_PORT, (unsigned)TIME_PORT);
    reply(fd, "200 OK", cseq, exBuf);
  }
  else if(!strcmp(method, "RECORD")){
    _released = false; _outReq = false; _idleSent = false;
    _reset();
    _lastPkt = millis();
    snprintf(exBuf + ex, 1024 - ex, "Audio-Latency: 11025\r\n");
    reply(fd, "200 OK", cseq, exBuf);
  }
  else if(!strcmp(method, "FLUSH")){
    _released = false; _outReq = false;
    _reset();
    reply(fd, "200 OK", cseq, exBuf);
  }
  else if(!strcmp(method, "SET_PARAMETER")){
    char ct[64] = {0};
    hget(req, hl, "Content-Type", ct, sizeof(ct));
    if(body && strstr(ct, "text/parameters")){
      const char* v = strstr(body, "volume:");
      if(v){
        volDb = atof(v + 7);
        int vol = volDb <= -30.0f ? 0 : volDb >= 0.0f ? 254 : (int)((volDb + 30.0f) * 254.0f / 30.0f + 0.5f);
        if(!_released) player.setVol((uint8_t)vol);
      }
    }else if(body && strstr(ct, "application/x-dmap-tagged")){
      char t[96] = {0}, a[64] = {0};
      dmap((const uint8_t*)body, bl, t, sizeof(t), a, sizeof(a));
      if(t[0]){
        if(a[0]) snprintf(_title, sizeof(_title), "%s - %s", a, t);
        else strlcpy(_title, t, sizeof(_title));
        _titleNew = true;
      }
    }
    reply(fd, "200 OK", cseq, exBuf);
  }
  else if(!strcmp(method, "GET_PARAMETER")){
    char b[40] = {0};
    if(body && strstr(body, "volume")){
      snprintf(b, sizeof(b), "volume: %.6f\r\n", volDb);
      snprintf(exBuf + ex, 1024 - ex, "Content-Type: text/parameters\r\n");
    }
    reply(fd, "200 OK", cseq, exBuf, b[0] ? b : nullptr);
  }
  else if(!strcmp(method, "TEARDOWN")){
    snprintf(exBuf + ex, 1024 - ex, "Connection: close\r\n");
    reply(fd, "200 OK", cseq, exBuf);
    if(cur == owner) _end();
  }
  else if(!strcmp(method, "POST") && strstr(req, "/feedback")){
    reply(fd, "200 OK", cseq, exBuf);
  }
  else{
    reply(fd, "501 Not Implemented", cseq, exBuf);
  }
}

/*  ---------- звук ---------- */

void YoAirplay::_packet(uint8_t* p, uint16_t n, uint8_t kind){
  if(n < 16) return;
  uint8_t type = p[1] & 0x7F;
  if(type == 0x56){                                   /* повтор загубленого: усередині звичайний пакет */
    if(n < 4 + 16) return;
    p += 4; n -= 4; type = 0x60;
  }else if(kind == 1) return;                          /* синхронізація (0x54) — нам не потрібна */
  if(type != 0x60 || !_session || _released || owner < 0) return;
  const uint16_t seq = (uint16_t)((p[2] << 8) | p[3]);
  const uint8_t* pl = p + 12;
  const uint16_t len = n - 12;
  if(enc){
    const uint16_t al = len & ~15;
    uint8_t iv[16];
    memcpy(iv, aesIv, 16);
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, al, iv, pl, decBuf);
    memcpy(decBuf + al, pl + al, len - al);
  }else memcpy(decBuf, pl, len);
  memset(decBuf + len, 0, 16);                         /* декодер може зазирнути трохи за кінець */
  int frames = 0;
  if(isAlac){
    if(alacDec){ int ob = FPP * 4; alac_decode_frame(alacDec, decBuf, tmpPcm, &ob); frames = ob / 4; }
  }else{
    frames = (len < FPP * 4 ? len : FPP * 4) / 4;
    for(int k = 0; k < frames * 2; k++) tmpPcm[k] = (int16_t)((decBuf[k * 2] << 8) | decBuf[k * 2 + 1]);
  }
  if(frames <= 0){ stBad++; return; }
  _store(seq, tmpPcm, (uint16_t)frames);
  _lastPkt = millis();
  _idleSent = false;
  if(!player.extOn && (!_outReq || millis() - outReqAt > 2000)){
    _outReq = true; outReqAt = millis();
    player.sendCommand({PR_EXT, 1});
  }
}

void YoAirplay::_store(uint16_t seq, const int16_t* pcm, uint16_t frames){
  if(frames > FPP) frames = FPP;
  uint16_t missFrom = 0, missN = 0;
  xSemaphoreTake(mtx, portMAX_DELAY);
  if(!synced){ synced = true; filling = true; rd = seq; wr = seq; avgFill = (uint32_t)TARGET << 8; }
  const int16_t ahead = (int16_t)(uint16_t)(seq - rd);
  if(ahead < 0){ stLate++; xSemaphoreGive(mtx); return; }
  if(ahead >= NSLOT - 1){
    /*  Далеко попереду (довга пауза в мережі) — починаємо заново звідси.  */
    for(uint16_t i = 0; i < NSLOT; i++) slot[i].ready = 0;
    rd = seq; wr = seq; filling = true; stSkip++;
  }
  const int16_t gap = (int16_t)(uint16_t)(seq - wr);
  if(gap >= 0){
    if(gap > 0 && gap < 64){ missFrom = wr; missN = (uint16_t)gap; }
    wr = seq + 1;
  }
  const uint16_t idx = seq % NSLOT;
  memcpy(&slotPcm[(size_t)idx * FPP * 2], pcm, (size_t)frames * 4);
  slot[idx].seq = seq; slot[idx].frames = frames; slot[idx].ready = 1;
  xSemaphoreGive(mtx);
  stPk++;
  if(missN) _resend(missFrom, missN);
}

void YoAirplay::_resend(uint16_t first, uint16_t count){
  if(!cport) return;
  const uint8_t q[8] = { 0x80, 0xD5, 0, 1, (uint8_t)(first >> 8), (uint8_t)first, (uint8_t)(count >> 8), (uint8_t)count };
  udpSend(pcbC, q, sizeof(q), cport);
  stResend += count;
}

void YoAirplay::_timing(){
  if(!tport) return;
  const uint32_t iv = timingN < 3 ? 300 : 3000;
  if(lastTiming && millis() - lastTiming < iv) return;
  lastTiming = millis();
  if(timingN < 3) timingN++;
  uint8_t q[32] = {0};
  q[0] = 0x80; q[1] = 0xD2; q[3] = 7;
  ntpNow(q + 24);
  udpSend(pcbT, q, sizeof(q), tport);
}

void YoAirplay::_reset(){
  if(!mtx) return;
  xSemaphoreTake(mtx, portMAX_DELAY);
  synced = false; filling = true;
  if(slot) for(uint16_t i = 0; i < NSLOT; i++) slot[i].ready = 0;
  xSemaphoreGive(mtx);
}

void YoAirplay::_end(){
  if(!_session) return;
  _session = false;
  owner = -1;
  cport = tport = 0;
  _reset();
  _outReq = false;
  if(player.extOn) player.sendCommand({PR_EXT, 0});
  Serial.printf("##AIRPLAY#\tсесію завершено (пакетів %u, загублено %u, повторів %u)\n", (unsigned)stPk, (unsigned)stLost, (unsigned)stResend);
}

/*  ---------- для плеєра ---------- */

size_t YoAirplay::read(int16_t* out, size_t maxFrames){
  if(!mtx || !slot || !_session || _released || maxFrames < FPP + 1) return 0;
  xSemaphoreTake(mtx, portMAX_DELAY);
  if(!synced){ xSemaphoreGive(mtx); return 0; }
  uint16_t fill = (uint16_t)(wr - rd);
  if(filling){
    if(fill < TARGET){ xSemaphoreGive(mtx); return 0; }
    filling = false;
    avgFill = (uint32_t)fill << 8;
  }
  if(fill == 0){ filling = true; stUnder++; xSemaphoreGive(mtx); return 0; }
  if(fill > TARGET + 200){
    /*  Після затору в мережі набралось забагато — наздоганяємо, щоб звук не відставав.  */
    const uint16_t skip = fill - TARGET;
    for(uint16_t k = 0; k < skip; k++) slot[(uint16_t)(rd + k) % NSLOT].ready = 0;
    rd += skip; fill = TARGET; stSkip += skip;
  }
  const uint16_t idx = rd % NSLOT;
  size_t n;
  if(slot[idx].ready && slot[idx].seq == rd){
    n = slot[idx].frames;
    memcpy(out, &slotPcm[(size_t)idx * FPP * 2], n * 4);
    slot[idx].ready = 0;
  }else if(fill > 8){
    n = FPP; memset(out, 0, n * 4); stLost++;          /* не дочекались повтору — тиша замість пакета */
  }else{
    xSemaphoreGive(mtx);
    return 0;                                           /* ще може прийти повтором */
  }
  rd++;
  avgFill = avgFill - (avgFill >> 8) + fill;
  xSemaphoreGive(mtx);
  /*  Годинники телефона й радіо трохи розходяться: раз на 8 пакетів, якщо
      запас стабільно завеликий чи замалий, прибираємо чи повторюємо один відлік.  */
  if(++stuffCnt >= 8){
    const uint32_t a = avgFill >> 8;
    if(a > TARGET + 16 && n > 1){ n--; stuffCnt = 0; stDrop++; }
    else if(a + 16 < TARGET && n){ out[n * 2] = out[n * 2 - 2]; out[n * 2 + 1] = out[n * 2 - 1]; n++; stuffCnt = 0; stAdd++; }
  }
  return n;
}

void YoAirplay::released(){
  _released = true;
  _outReq = false;
}

bool YoAirplay::takeTitle(char* out, size_t n){
  if(!_titleNew) return false;
  _titleNew = false;
  strlcpy(out, _title, n);
  return true;
}

void YoAirplay::stat(){
  Serial.printf("airplay: увімкнено=%d ключ=%d (%s) видно=%d сесія=%d плеєр=%d відпущено=%d пристрій=«%s»\n",
                on(), (int)_keyOk, _keyErr, (int)_mdnsOn, (int)_session, (int)player.extOn, (int)_released, _device);
  Serial.printf("  пакетів %u, запізнилось %u, загублено %u, повторів %u, зіпсовано %u, черга мережі губила %u\n",
                (unsigned)stPk, (unsigned)stLate, (unsigned)stLost, (unsigned)stResend, (unsigned)stBad, (unsigned)stRawDrop);
  Serial.printf("  буфер %u пакетів (середнє %u, ціль %u), порожньо %u, пропущено %u, +відліків %u, -відліків %u\n",
                (unsigned)(uint16_t)(wr - rd), (unsigned)(avgFill >> 8), (unsigned)TARGET, (unsigned)stUnder, (unsigned)stSkip, (unsigned)stAdd, (unsigned)stDrop);
  if(_task) Serial.printf("  стек задачі: вільно %u Б\n", (unsigned)uxTaskGetStackHighWaterMark((TaskHandle_t)_task));
}
