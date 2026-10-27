/*  Консоль налагодження по Wi-Fi — див. yoNetCon.h.  */
#define YO_NETCON_IMPL
#include "yoNetCon.h"
#include <WiFi.h>
#include "esp_heap_caps.h"
#include "yoExtras.h"             /* extras.s.noNetCon — вимикач у «Розробнику» */
#include "yoVersion.h"

YoTee yoTee;

#define NC_RING (64 * 1024)       /* журнал у PSRAM: що радіо писало останнім */
#define NC_IN   1024              /* рядки від клієнта, ще не прочитані консоллю */

static uint8_t* s_ring = nullptr;
static volatile uint32_t s_head = 0;              /* байтів записано за весь час — кінець журналу */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char* s_in = nullptr;
static volatile uint16_t s_inW = 0, s_inR = 0;
static volatile bool s_peer = false;
static char s_peerIp[16] = {0};
static volatile uint32_t s_sent = 0, s_skipped = 0, s_conns = 0, s_stackFree = 0;

/*  Запис у кільце — з будь-якої задачі; мережі не чекає ніколи.  */
static void ringPut(const uint8_t* b, size_t n){
  if(!s_ring || !n) return;
  if(n > NC_RING){ b += n - NC_RING; n = NC_RING; }
  portENTER_CRITICAL(&s_mux);
  const uint32_t h = s_head, o = h % NC_RING, k = (NC_RING - o) < n ? (NC_RING - o) : (uint32_t)n;
  memcpy(s_ring + o, b, k);
  if(n > k) memcpy(s_ring, b + k, n - k);
  s_head = h + n;
  portEXIT_CRITICAL(&s_mux);
}

void YoTee::begin(unsigned long baud){
  YO_REAL_SERIAL.begin(baud);
  if(!s_ring) s_ring = (uint8_t*)heap_caps_malloc(NC_RING, MALLOC_CAP_SPIRAM);
  if(!s_in) s_in = (char*)heap_caps_malloc(NC_IN, MALLOC_CAP_SPIRAM);
}
size_t YoTee::write(uint8_t c){ ringPut(&c, 1); YO_REAL_SERIAL.write(c); return 1; }
size_t YoTee::write(const uint8_t* b, size_t n){ ringPut(b, n); YO_REAL_SERIAL.write(b, n); return n; }
int YoTee::available(){ return YO_REAL_SERIAL.available() + (uint16_t)(s_inW - s_inR); }
int YoTee::read(){
  if(YO_REAL_SERIAL.available()) return YO_REAL_SERIAL.read();
  if(s_in && s_inR != s_inW){ const uint8_t c = (uint8_t)s_in[s_inR % NC_IN]; s_inR = s_inR + 1; return c; }
  return -1;
}
int YoTee::peek(){
  if(YO_REAL_SERIAL.available()) return YO_REAL_SERIAL.peek();
  if(s_in && s_inR != s_inW) return (uint8_t)s_in[s_inR % NC_IN];
  return -1;
}
void YoTee::flush(){ YO_REAL_SERIAL.flush(); }
int YoTee::availableForWrite(){ return YO_REAL_SERIAL.availableForWrite(); }
YoTee::operator bool() const { return (bool)YO_REAL_SERIAL; }

/*  Лише з тієї ж підмережі, що й радіо: з інтернету (через переадресацію порту) — ні.  */
static bool sameNet(IPAddress a){
  const IPAddress me = WiFi.localIP(), m = WiFi.subnetMask();
  for(uint8_t i = 0; i < 4; i++) if((a[i] & m[i]) != (me[i] & m[i])) return false;
  return true;
}

/*  Журнал клієнтові: з його позиції rd до кінця. Відстав на ціле кільце — пропуск із позначкою.  */
static void ncSend(NetworkClient& cl, uint32_t& rd){
  const uint32_t h = s_head;
  if(h - rd > NC_RING - 4096){
    const uint32_t nr = h - NC_RING / 2;
    s_skipped += nr - rd;
    char m[64]; const int k = snprintf(m, sizeof(m), "\n##NETCON#\tпропущено %u Б журналу\n", (unsigned)(nr - rd));
    cl.write((const uint8_t*)m, k);
    rd = nr;
  }
  for(uint8_t i = 0; i < 32 && rd != h; i++){
    const uint32_t o = rd % NC_RING;
    uint32_t k = h - rd;
    if(k > NC_RING - o) k = NC_RING - o;
    if(k > 1436) k = 1436;
    const int w = cl.write(s_ring + o, k);
    if(w <= 0) break;
    rd += w; s_sent += w;
  }
}

static void ncTask(void*){
  NetworkServer srv(NETCON_PORT);
  NetworkClient cl;
  bool up = false;
  uint32_t rd = 0;
  char line[200]; uint8_t ln = 0;
  for(;;){
    const bool on = !extras.s.noNetCon && WiFi.status() == WL_CONNECTED && s_ring && s_in;
    if(!on){
      if(s_peer){ cl.stop(); s_peer = false; }
      if(up){ srv.end(); up = false; }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if(!up){ srv.begin(); srv.setNoDelay(true); up = true; }
    if(srv.hasClient()){
      NetworkClient n = srv.accept();
      if(n && sameNet(n.remoteIP())){
        if(s_peer){ const char* m = "##NETCON#\tпід'єднався інший клієнт — до побачення\n"; cl.write((const uint8_t*)m, strlen(m)); cl.stop(); }
        cl = n; cl.setNoDelay(true);
        rd = s_head; ln = 0; s_conns++;
        strlcpy(s_peerIp, cl.remoteIP().toString().c_str(), sizeof(s_peerIp));
        s_peer = true;
        char g[200];
        const int k = snprintf(g, sizeof(g), "##NETCON#\tПОТУЖНЕ РАДІО %s (%s) — консоль налагодження. «hist» — журнал із пам'яті, «bye» — вийти\n",
                               prVersion(), prBuild());
        cl.write((const uint8_t*)g, k);
        yoTee.printf("##NETCON#\tпід'єднався %s\n", s_peerIp);
      }else if(n) n.stop();
    }
    if(s_peer){
      if(!cl.connected()){
        cl.stop(); s_peer = false;
        yoTee.printf("##NETCON#\tвід'єднався %s\n", s_peerIp);
      }else{
        while(cl.available()){
          const int c = cl.read();
          if(c < 0) break;
          if(c == '\r') continue;
          if(c != '\n'){ if(ln < sizeof(line) - 1) line[ln++] = (char)c; continue; }
          line[ln] = 0; ln = 0;
          if(!strcmp(line, "hist")){ const uint32_t h = s_head; rd = h > NC_RING - 4096 ? h - (NC_RING - 4096) : 0; continue; }
          if(!strcmp(line, "bye")){ cl.stop(); s_peer = false; yoTee.printf("##NETCON#\tвід'єднався %s\n", s_peerIp); break; }
          /*  у вхід консолі: menu/yoDebug.cpp читає його так само, як USB  */
          const size_t L = strlen(line);
          if((uint16_t)(s_inW - s_inR) + L + 1 < NC_IN){
            uint16_t w = s_inW;
            for(size_t i = 0; i < L; i++){ s_in[w % NC_IN] = line[i]; w++; }
            s_in[w % NC_IN] = '\n'; w++;
            __sync_synchronize();
            s_inW = w;
          }
        }
        if(s_peer) ncSend(cl, rd);
      }
    }
    s_stackFree = uxTaskGetStackHighWaterMark(nullptr);
    vTaskDelay(pdMS_TO_TICKS(s_peer ? 10 : 100));
  }
}

void yoNetConBegin(){
  static bool started = false;
  if(started) return;
  started = true;
  if(xTaskCreatePinnedToCore(ncTask, "netcon", 6144, nullptr, 1, nullptr, 0) != pdPASS) started = false;
}

bool yoNetConPeer(char* ip, size_t cap){
  if(!s_peer) return false;
  if(ip && cap) strlcpy(ip, s_peerIp, cap);
  return true;
}

void yoNetConInfo(Print& o){
  o.printf("NETCON порт %u, %s; клієнт %s; надіслано %u Б, пропущено %u Б, з'єднань %u; у журналі %u Б з %u; стек вільний %u Б\n",
           (unsigned)NETCON_PORT, extras.s.noNetCon ? "ВИМКНЕНО" : "увімкнено", s_peer ? s_peerIp : "—",
           (unsigned)s_sent, (unsigned)s_skipped, (unsigned)s_conns,
           (unsigned)(s_head < NC_RING ? s_head : NC_RING), (unsigned)NC_RING, (unsigned)s_stackFree);
}
