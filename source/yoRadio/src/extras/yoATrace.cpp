/*  Запис огинаючої звуку — див. yoATrace.h.  */
#include "../core/options.h"      /* тут Serial підміняється «трійником» — інакше друк ішов би лише в USB, повз консоль по Wi-Fi */
#include "yoATrace.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <math.h>

namespace atrace {

volatile bool on = false;

struct Bin { uint32_t t; uint16_t in, out, fs; };
struct Mic { uint32_t t; uint16_t pk; };
struct Ev  { uint32_t t; const char* w; int32_t v; };
static const uint16_t NB = 6000, NM = 1500, NE = 96;
static Bin* s_bin = nullptr;
static Mic* s_mic = nullptr;
static Ev   s_ev[NE];
static volatile uint16_t s_nb = 0, s_nm = 0, s_ne = 0;
static int64_t  s_t0 = 0;
static uint32_t s_dur = 0;
static uint16_t s_cnt = 0, s_in = 0, s_out = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static inline uint32_t rel(){ return (uint32_t)(esp_timer_get_time() - s_t0); }

void arm(uint32_t ms){
  if(!s_bin) s_bin = (Bin*)heap_caps_malloc(sizeof(Bin) * NB, MALLOC_CAP_SPIRAM);
  if(!s_mic) s_mic = (Mic*)heap_caps_malloc(sizeof(Mic) * NM, MALLOC_CAP_SPIRAM);
  if(!s_bin || !s_mic){ Serial.println("ATRACE: немає пам'яті"); return; }
  on = false;
  s_nb = s_nm = s_ne = 0; s_cnt = s_in = s_out = 0;
  s_dur = (ms < 500 ? 500 : ms > 8000 ? 8000 : ms) * 1000UL;
  s_t0 = esp_timer_get_time();
  on = true;
  Serial.printf("ATRACE: пишу %u мс\n", (unsigned)(s_dur / 1000));
}

void sample(int16_t inL, int16_t inR, uint32_t out, uint32_t fs){
  const uint16_t a = (uint16_t)(abs((int)inL) > abs((int)inR) ? abs((int)inL) : abs((int)inR));
  const int16_t ol = (int16_t)(out >> 16), orr = (int16_t)(out & 0xFFFF);
  const uint16_t b = (uint16_t)(abs((int)ol) > abs((int)orr) ? abs((int)ol) : abs((int)orr));
  if(a > s_in) s_in = a;
  if(b > s_out) s_out = b;
  if(++s_cnt < 64) return;                         /* відмітка часу — раз на 64 відліки (~1,4 мс) */
  const uint32_t t = rel();
  if(t > s_dur){ on = false; return; }
  if(s_nb < NB){ s_bin[s_nb] = { t, s_in, s_out, (uint16_t)(fs / 10) }; s_nb = s_nb + 1; }
  s_cnt = 0; s_in = 0; s_out = 0;
}

void mic(int32_t peak){
  if(!on) return;
  const uint32_t t = rel();
  if(s_nm < NM){ s_mic[s_nm] = { t, (uint16_t)(peak > 32767 ? 32767 : peak) }; s_nm = s_nm + 1; }
}

void ev(const char* what, int32_t v){
  if(!on) return;
  portENTER_CRITICAL(&s_mux);
  if(s_ne < NE){ s_ev[s_ne] = { rel(), what, v }; s_ne = s_ne + 1; }
  portEXIT_CRITICAL(&s_mux);
}

static int8_t db(uint16_t v){ return v ? (int8_t)lroundf(20.0f * log10f(v / 32768.0f)) : -99; }

void dump(){
  on = false;
  Serial.printf("ATRACE_BEGIN бінів %u, мікрофон %u, подій %u, тривалість %u мс\n", (unsigned)s_nb, (unsigned)s_nm, (unsigned)s_ne, (unsigned)(s_dur / 1000));
  for(uint16_t i = 0; i < s_ne; i++) Serial.printf("AE %u.%u %s %d\n", (unsigned)(s_ev[i].t / 1000), (unsigned)(s_ev[i].t / 100 % 10), s_ev[i].w, (int)s_ev[i].v);
  /*  по 10 мс: найбільше з бінів і читань мікрофона, що потрапили в проміжок  */
  uint16_t bi = 0, mi = 0;
  for(uint32_t t = 0; t < s_dur; t += 10000){
    uint16_t in = 0, out = 0, fs = 0, mk = 0; bool any = false, anyM = false;
    while(bi < s_nb && s_bin[bi].t < t + 10000){ const Bin& b = s_bin[bi++]; if(b.in > in) in = b.in; if(b.out > out) out = b.out; fs = b.fs; any = true; }
    while(mi < s_nm && s_mic[mi].t < t + 10000){ if(s_mic[mi].pk > mk) mk = s_mic[mi].pk; mi++; anyM = true; }
    if(!any && !anyM) continue;
    Serial.printf("AT %u %d %d %d %u\n", (unsigned)(t / 1000), any ? db(in) : -100, any ? db(out) : -100, anyM ? db(mk) : -100, (unsigned)fs * 10);
  }
  Serial.println("ATRACE_END");
}

}  // namespace atrace
