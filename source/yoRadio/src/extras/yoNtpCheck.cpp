/*  Перевірка часу з мережі — опис і правила в yoNtpCheck.h.  */
#include "yoNtpCheck.h"
#include "yoNetCon.h"            /* Serial → консоль по Wi-Fi і журнал hist (без цього рядки йшли лише в USB) */
#include <sys/time.h>
#include <unistd.h>
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#define NC_JUMP_US      (60LL * 1000000)          /* відомий час змінюється більш ніж на хвилину — спершу друга думка */
#define NC_AGREE_US     (5LL * 1000000)           /* два джерела «згодні» */
#define NC_REPEAT_US    (4LL * 3600 * 1000000)    /* той самий стрибок удруге поспіль (SNTP питає раз на 3 год) */
#define NC_REPLY_MS     1500                      /* чекати відповідь одного сервера */
#define NC_MAXDIST_MS   1500                      /* відстань до еталона (root delay/2 + root dispersion) — як MAXDIST у ntpd */
#define NC_REF_AGE_S    86400                     /* еталон сервера не оновлювався довше доби — сервер втратив синхронізацію */
#define NC_TIME_VALID   1700000000                /* раніше — годинник часу не знає (після ввімкнення — 1970) */
#define NTP_UNIX_1900   2208988800UL              /* секунд від 1900 (епоха NTP) до 1970 (Unix) */

/*  друга думка: anycast-сервери великих мереж, не волонтерський пул (з нього приходить і сам SNTP)  */
static const char* const VERIFY[] = { "time.google.com", "time.cloudflare.com", "time.apple.com" };
#define VERIFY_N (sizeof(VERIFY) / sizeof(VERIFY[0]))

/* ---------- запит NTP одному серверу ---------- */
struct Reply {
  const char* host;
  const char* why;           /* чому відкинуто чи не вдалося ("" — придатна) */
  uint32_t ip;               /* s_addr: перший октет — молодший байт */
  int64_t utc_us;            /* час сервера в мить приходу відповіді (з половиною кола) */
  int64_t mono_us;           /* esp_timer_get_time() у мить приходу */
  uint32_t rtt_ms, dist_ms;
  uint8_t li, vn, mode, stratum;
};

static uint32_t be32(const uint8_t* p){ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/*  Розбір за RFC 4330/5905 (як fx_ntp_parse у годиннику). -1 — не відповідь на наш запит (коротка, чужа мітка):
    чекати далі; 0 — наша, але час брати не можна (r.why); 1 — придатна (sec/frac — час відправлення сервером).  */
static int8_t parse(const uint8_t* p, size_t n, const uint8_t* reqTx, Reply& r, uint32_t& sec, uint32_t& frac){
  static char kod[12];
  if(n < 48) return -1;
  if(memcmp(p + 24, reqTx, 8) != 0) return -1;       /* originate ≠ наш transmit: запізніла чи чужа відповідь */
  r.li = p[0] >> 6; r.vn = (p[0] >> 3) & 7; r.mode = p[0] & 7; r.stratum = p[1];
  const uint32_t rdelay = be32(p + 4), rdisp = be32(p + 8), ref = be32(p + 16);
  sec = be32(p + 40); frac = be32(p + 44);
  const uint64_t dist = (rdelay & 0x80000000UL) ? 0xffffffffULL : ((uint64_t)rdelay * 500 >> 16) + ((uint64_t)rdisp * 1000 >> 16);
  r.dist_ms = dist > 0xffffffffULL ? 0xffffffffUL : (uint32_t)dist;
  if(r.mode != 4){ r.why = "режим не 4"; return 0; }
  if(r.stratum == 0){                                  /* «поцілунок смерті»: сервер просить не питати, код у refid */
    snprintf(kod, sizeof(kod), "KoD %c%c%c%c", isprint(p[12]) ? p[12] : '?', isprint(p[13]) ? p[13] : '?',
             isprint(p[14]) ? p[14] : '?', isprint(p[15]) ? p[15] : '?');
    r.why = kod; return 0;
  }
  if(r.li == 3){ r.why = "LI=3, не синхронізований"; return 0; }
  if(r.stratum > 15){ r.why = "страта понад 15"; return 0; }
  if(r.vn < 1 || r.vn > 4){ r.why = "версія"; return 0; }
  if(sec == 0){ r.why = "без часу"; return 0; }
  if(ref == 0){ r.why = "ще не синхронізувався"; return 0; }
  if(r.dist_ms > NC_MAXDIST_MS){ r.why = "далеко від еталона"; return 0; }
  if((int32_t)(sec - ref) > NC_REF_AGE_S){ r.why = "еталон застарів"; return 0; }
  return 1;
}

/*  дві адреси з однієї мережі /16 — не незалежна перевірка (у годиннику: пул дав 162.159.200.123, а «другим»
    став time.cloudflare.com = 162.159.200.1, той самий Cloudflare)  */
static bool sameNet(uint32_t a, uint32_t b){ return a && b && (a & 0xffff) == (b & 0xffff); }

static const char* ipTxt(uint32_t ip, char* b, size_t n){
  snprintf(b, n, "%u.%u.%u.%u", (unsigned)(ip & 0xff), (unsigned)(ip >> 8 & 0xff), (unsigned)(ip >> 16 & 0xff), (unsigned)(ip >> 24));
  return b;
}

/*  1 — придатна відповідь; 0 — сервер відповів, але відкинуто (r.why); -1 — немає відповіді; -2 — пропущено
    (та сама мережа /16, що й у когось із avoid)  */
static int8_t ask(const char* host, const uint32_t* avoid, uint8_t nAvoid, Reply& r){
  memset(&r, 0, sizeof(r));
  r.host = host; r.why = "";
  struct addrinfo hints; memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
  struct addrinfo* ai = nullptr;
  if(getaddrinfo(host, "123", &hints, &ai) != 0 || !ai){ r.why = "DNS"; return -1; }
  r.ip = ((struct sockaddr_in*)ai->ai_addr)->sin_addr.s_addr;
  freeaddrinfo(ai);
  for(uint8_t i = 0; i < nAvoid; i++) if(sameNet(r.ip, avoid[i])){ r.why = "та сама мережа /16"; return -2; }
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if(s < 0){ r.why = "сокет"; return -1; }
  uint8_t req[48], buf[68];
  memset(req, 0, sizeof(req));
  req[0] = 0x23;                                       /* LI 0, версія 4, режим 3 (клієнт) */
  const uint32_t n1 = esp_random(), n2 = esp_random(); /* мітка запиту випадкова: відповідь мусить її повернути */
  memcpy(req + 40, &n1, 4); memcpy(req + 44, &n2, 4);
  struct sockaddr_in to; memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET; to.sin_port = htons(123); to.sin_addr.s_addr = r.ip;
  int8_t res = -1;
  uint32_t sec = 0, frac = 0;
  const int64_t t1 = esp_timer_get_time();
  if(sendto(s, req, sizeof(req), 0, (struct sockaddr*)&to, sizeof(to)) == (int)sizeof(req)){
    r.why = "немає відповіді";
    for(;;){
      const int32_t left = NC_REPLY_MS - (int32_t)((esp_timer_get_time() - t1) / 1000);
      if(left <= 0) break;
      struct timeval tv = { left / 1000, (left % 1000) * 1000 };
      setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      struct sockaddr_in from; socklen_t fl = sizeof(from);
      const int n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr*)&from, &fl);
      const int64_t t2 = esp_timer_get_time();
      if(n <= 0) break;                                /* тайм-аут */
      if(from.sin_addr.s_addr != r.ip || from.sin_port != htons(123)) continue;   /* чужа адреса */
      const int8_t k = parse(buf, (size_t)n, req + 40, r, sec, frac);
      if(k < 0) continue;                              /* не відповідь на цей запит — чекаємо далі */
      res = k; r.mono_us = t2; break;
    }
  }else r.why = "не відправилось";
  close(s);
  if(res >= 0){
    const int64_t rtt = r.mono_us - t1;
    r.rtt_ms = (uint32_t)(rtt / 1000);
    /*  час у мить приходу ≈ час відправлення + половина кола  */
    r.utc_us = (int64_t)(uint32_t)(sec - NTP_UNIX_1900) * 1000000 + (int64_t)(((uint64_t)frac * 1000000ULL) >> 32) + rtt / 2;
  }
  return res;
}

/* ---------- стан ---------- */
struct Cand {                                          /* час від SNTP, що чекає перевірки */
  int64_t utc_us, mono_us, clk_us;                     /* кандидат, мить приходу, показ годинника в ту мить */
  bool known;                                          /* годинник знав час */
  uint8_t reach[3];                                    /* досяжність серверів SNTP до цієї відповіді */
  uint32_t ip[3];
  char name[3][40];
};
static Cand s_c;
static volatile bool s_busy = false;                   /* перевірка йде (задача ntpChk) */
static char s_verifyBuf[48];
static const char* volatile s_verify = nullptr;        /* консоль «ntpverify»: друга думка лише від цього */
static bool s_pend = false;                            /* відкладений стрибок (другого сервера не було) */
static int64_t s_pendD = 0, s_pendMono = 0;
static uint32_t s_nDirect, s_nCheck, s_nOk, s_nRej, s_nNoCheck, s_nKept, s_nBusy;
static int64_t s_lastDirAt = 0, s_lastDirD = 0;        /* остання звичайна звірка: коли (UTC, мкс) і поправка */
static portMUX_TYPE s_txtMux = portMUX_INITIALIZER_UNLOCKED;
static char s_last[240];                               /* остання перевірка — для консолі */

static int64_t sysUs(){ struct timeval t; gettimeofday(&t, nullptr); return (int64_t)t.tv_sec * 1000000 + t.tv_usec; }
static void setUs(int64_t us){
  struct timeval t; t.tv_sec = (time_t)(us / 1000000); t.tv_usec = (suseconds_t)(us % 1000000);
  settimeofday(&t, nullptr);
  sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
}
static int64_t absUs(int64_t v){ return v < 0 ? -v : v; }

/*  різниця для людини: «+6 хв 15 с», «-12 год 23 хв»  */
static const char* dTxt(int64_t us, char* b, size_t n){
  const char sg = us < 0 ? '-' : '+';
  int64_t s = absUs(us) / 1000000;
  if(s < 60) snprintf(b, n, "%c%lld с", sg, (long long)s);
  else if(s < 3600) snprintf(b, n, "%c%lld хв %lld с", sg, (long long)(s / 60), (long long)(s % 60));
  else if(s < 86400LL * 2) snprintf(b, n, "%c%lld год %lld хв", sg, (long long)(s / 3600), (long long)(s % 3600 / 60));
  else snprintf(b, n, "%c%lld діб", sg, (long long)(s / 86400));
  return b;
}
/*  місцевий час для людини: «17:03:12 15.11»  */
static const char* tTxt(int64_t us, char* b, size_t n){
  const time_t t = (time_t)(us / 1000000); struct tm lt; localtime_r(&t, &lt);
  snprintf(b, n, "%02d:%02d:%02d %02d.%02d.%04d", lt.tm_hour, lt.tm_min, lt.tm_sec, lt.tm_mday, lt.tm_mon + 1, lt.tm_year + 1900);
  return b;
}
static void note(const char* fmt, ...){
  char b[sizeof(s_last)];
  va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
  Serial.printf("##TIME#\t%s\n", b);
  char when[24]; tTxt(sysUs(), when, sizeof(when));
  portENTER_CRITICAL(&s_txtMux);
  snprintf(s_last, sizeof(s_last), "%s: %s", when, b);
  portEXIT_CRITICAL(&s_txtMux);
}

/* ---------- перевірка (своя задача: запити NTP блокуючі) ---------- */
static void checkTask(void*){
  vTaskDelay(pdMS_TO_TICKS(50));                       /* lwIP після нашого виклику позначить, який сервер відповів */
  const Cand c = s_c;
  int8_t src = -1;                                     /* джерело: сервер, у якого біт «відповів» щойно став 1 */
  for(uint8_t i = 0; i < 3; i++) if(!(c.reach[i] & 1) && (esp_sntp_getreachability(i) & 1)){ src = (int8_t)i; break; }
  char srcTxt[64], ipb[16];
  if(src >= 0){
    ipTxt(c.ip[src], ipb, sizeof(ipb));
    if(!strcmp(c.name[src], ipb) || !c.ip[src]) strlcpy(srcTxt, c.name[src][0] ? c.name[src] : ipb, sizeof(srcTxt));
    else snprintf(srcTxt, sizeof(srcTxt), "%s %s", c.name[src], ipb);
  }else strlcpy(srcTxt, "SNTP", sizeof(srcTxt));
  const int64_t dC = c.known ? c.utc_us - c.clk_us : 0; /* SNTP проти годинника */
  char d1[32], t1[24], t2[24];

  const char* list[VERIFY_N]; uint8_t nList = 0;
  if(const char* v = s_verify) list[nList++] = v;
  else for(uint8_t i = 0; i < VERIFY_N; i++) list[nList++] = VERIFY[i];
  uint32_t avoid[VERIFY_N + 1]; uint8_t nAvoid = 0;
  if(src >= 0 && c.ip[src]) avoid[nAvoid++] = c.ip[src];
  char miss[160] = "";                                 /* хто не відповів чи відкинутий — для журналу */
  Reply r, prev; bool havePrev = false;
  enum { NONE, BY_R, BY_C } act = NONE;
  enum { H_OK, H_CLOCK, H_TWO } how = H_OK;
  for(uint8_t i = 0; i < nList && act == NONE; i++){
    const int8_t k = ask(list[i], avoid, nAvoid, r);
    if(k != 1){
      const size_t l = strlen(miss);
      snprintf(miss + l, sizeof(miss) - l, "%s%s %s", l ? "; " : "", r.host, r.why);
      continue;
    }
    if(absUs(r.utc_us - (c.utc_us + (r.mono_us - c.mono_us))) <= NC_AGREE_US){ act = BY_R; how = H_OK; break; }
    if(c.known && absUs(r.utc_us - (c.clk_us + (r.mono_us - c.mono_us))) <= NC_JUMP_US){ act = BY_R; how = H_CLOCK; break; }
    if(havePrev && absUs(r.utc_us - (prev.utc_us + (r.mono_us - prev.mono_us))) <= NC_AGREE_US){ act = BY_R; how = H_TWO; break; }
    prev = r; havePrev = true;
    if(nAvoid < VERIFY_N + 1) avoid[nAvoid++] = r.ip;
  }

  char rTxt[64] = "";
  if(act == BY_R) snprintf(rTxt, sizeof(rTxt), "%s %s", r.host, ipTxt(r.ip, ipb, sizeof(ipb)));
  if(act == BY_R){
    const int64_t now = r.utc_us + (esp_timer_get_time() - r.mono_us);
    setUs(now);
    s_pend = false;
    if(how == H_OK){
      s_nOk++;
      if(c.known) note("стрибок %s від %s підтвердив %s: %s -> %s", dTxt(dC, d1, sizeof(d1)), srcTxt, rTxt,
                       tTxt(c.clk_us + (esp_timer_get_time() - c.mono_us), t1, sizeof(t1)), tTxt(now, t2, sizeof(t2)));
      else note("час від %s підтвердив %s: %s", srcTxt, rTxt, tTxt(now, t1, sizeof(t1)));
    }else{
      s_nRej++;
      const int64_t bad = c.utc_us + (r.mono_us - c.mono_us) - r.utc_us;   /* наскільки помилявся SNTP */
      if(how == H_CLOCK)
        note("ВІДКИНУТО час від %s: %s від правильного (%s); %s згоден із годинником", srcTxt, dTxt(bad, d1, sizeof(d1)),
             tTxt(c.utc_us + (esp_timer_get_time() - c.mono_us), t1, sizeof(t1)), rTxt);
      else{
        char pb[16];
        note("ВІДКИНУТО час від %s: %s від правильного (%s); %s %s і %s згодні між собою — час %s", srcTxt, dTxt(bad, d1, sizeof(d1)),
             tTxt(c.utc_us + (esp_timer_get_time() - c.mono_us), t1, sizeof(t1)), prev.host, ipTxt(prev.ip, pb, sizeof(pb)), rTxt,
             tTxt(now, t2, sizeof(t2)));
      }
    }
  }else if(!havePrev){                                  /* другого сервера немає */
    const bool repeat = c.known && s_pend && esp_timer_get_time() - s_pendMono < NC_REPEAT_US && absUs(dC - s_pendD) <= NC_AGREE_US;
    if(!c.known || repeat){
      /*  спершу час, потім рядок: інакше в «остання перевірка» мітка ще з 1970 року  */
      const int64_t was = c.clk_us + (esp_timer_get_time() - c.mono_us);
      act = BY_C;
      setUs(c.utc_us + (esp_timer_get_time() - c.mono_us));
      s_pend = false;
      if(!c.known){
        s_nNoCheck++;
        note("час від %s перевірити не вдалося (%s) — прийнято без перевірки: %s", srcTxt, miss, tTxt(sysUs(), t1, sizeof(t1)));
      }else{
        s_nOk++;
        note("стрибок %s від %s удруге поспіль, другого сервера немає (%s) — час змінено: %s -> %s", dTxt(dC, d1, sizeof(d1)),
             srcTxt, miss, tTxt(was, t1, sizeof(t1)), tTxt(sysUs(), t2, sizeof(t2)));
      }
    }else{
      s_pend = true; s_pendD = dC; s_pendMono = esp_timer_get_time(); s_nKept++;
      note("стрибок %s від %s: другого сервера немає (%s) — час не змінено до наступної звірки", dTxt(dC, d1, sizeof(d1)), srcTxt, miss);
    }
  }else{                                                /* відповіді є, але згоди немає */
    s_nKept++;
    const int64_t pd = prev.utc_us - (c.utc_us + (prev.mono_us - c.mono_us));
    char d2[32], dk[48] = "";
    if(c.known) snprintf(dk, sizeof(dk), " (%s від годинника)", dTxt(dC, d1, sizeof(d1)));
    note("сервери розходяться: %s%s, а %s — %s від нього%s%s; час %s", srcTxt, dk, prev.host, dTxt(pd, d2, sizeof(d2)),
         miss[0] ? "; " : "", miss, c.known ? "не змінено" : "не поставлено, ще спроба за хвилину");
  }
  s_busy = false;
  vTaskDelete(nullptr);
}

/*  Заміна «слабкої» sntp_sync_time з lwIP (esp_sntp.h): її викликає SNTP ядра з потоку lwIP на кожну відповідь
    сервера. Тут — лише швидке: нічого не чекаємо, запити — у задачі ntpChk. Типова з ядра (режим IMMED) робить
    settimeofday і статус COMPLETED; зворотного виклику sntp_set_time_sync_notification_cb у радіо немає.  */
extern "C" void sntp_sync_time(struct timeval* tv){
  const int64_t c = (int64_t)tv->tv_sec * 1000000 + tv->tv_usec;
  const int64_t clk = sysUs();
  const bool known = clk / 1000000 > NC_TIME_VALID;
  if(known && absUs(c - clk) <= NC_JUMP_US){           /* звичайна звірка — як і раніше */
    settimeofday(tv, nullptr);
    sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
    s_nDirect++; s_pend = false;
    s_lastDirAt = c; s_lastDirD = c - clk;
    return;
  }
  if(s_busy){ s_nBusy++; return; }                    /* перевірка попередньої ще йде — SNTP спитає знову */
  s_nCheck++;
  Cand& k = s_c;                                       /* задачі немає (s_busy == false) — пишемо без замка */
  k.utc_us = c; k.mono_us = esp_timer_get_time(); k.clk_us = clk; k.known = known;
  for(uint8_t i = 0; i < 3; i++){
    k.reach[i] = esp_sntp_getreachability(i);          /* обгортки без замка: ми в потоці lwIP */
    const ip_addr_t* a = esp_sntp_getserver(i);
    k.ip[i] = (a && IP_IS_V4(a)) ? ip4_addr_get_u32(ip_2_ip4(a)) : 0;
    const char* nm = esp_sntp_getservername(i);
    strlcpy(k.name[i], nm ? nm : "", sizeof(k.name[i]));
  }
  s_busy = true;
  if(xTaskCreatePinnedToCore(checkTask, "ntpChk", 6144, nullptr, 1, nullptr, 0) != pdPASS){
    s_busy = false;
    if(!known){ settimeofday(tv, nullptr); sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED); }   /* як було */
    Serial.println("##TIME#\tперевірка часу: не вистачило пам'яті на задачу");
  }
}

namespace yoNtp {
void status(Print& out){
  char last[sizeof(s_last)];
  portENTER_CRITICAL(&s_txtMux);
  memcpy(last, s_last, sizeof(last));
  portEXIT_CRITICAL(&s_txtMux);
  char d[32];
  out.printf("NTPCHECK звірок без стрибка %lu; перевірок %lu: підтверджено %lu, відкинуто %lu, без перевірки %lu, "
             "не змінено %lu, пропущено (йшла перевірка) %lu; відкладений стрибок: %s; друга думка: %s\n",
             (unsigned long)s_nDirect, (unsigned long)s_nCheck, (unsigned long)s_nOk, (unsigned long)s_nRej,
             (unsigned long)s_nNoCheck, (unsigned long)s_nKept, (unsigned long)s_nBusy,
             s_pend ? dTxt(s_pendD, d, sizeof(d)) : "немає", s_verify ? (const char*)s_verify : "time.google.com, time.cloudflare.com, time.apple.com");
  out.printf("NTPCHECK остання перевірка: %s\n", last[0] ? last : "-");
  char t[24];
  if(s_lastDirAt) out.printf("NTPCHECK остання звичайна звірка: %s, поправка %+lld мс\n", tTxt(s_lastDirAt, t, sizeof(t)),
                             (long long)(s_lastDirD / 1000));
}
void setVerifier(const char* host){
  if(host && host[0]){ strlcpy(s_verifyBuf, host, sizeof(s_verifyBuf)); s_verify = s_verifyBuf; }
  else s_verify = nullptr;
}
}
