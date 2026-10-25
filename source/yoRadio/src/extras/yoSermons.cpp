#include "yoSermons.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "../core/options.h"
#include "../core/config.h"
#include "../core/player.h"
#include "../core/display.h"
#include "yoRecorder.h"
#include "../core/netserver.h"
#include "yoLogos.h"
#include "esp32s3/rom/tjpgd.h"

YoSermons sermons;

#define SM_OBJ   1800            /* найдовший об'єкт у відповіді — близько 700 байт */

/*  Якщо рядок обрізано посеред літери UTF-8, прибираємо недописаний
    хвіст. Цілу останню літеру не чіпаємо: попередня версія зрізала її
    завжди, і «Денис» ставав «Дени».  */
void utf8FixTail(char* s){
  size_t n = strlen(s);
  if(!n) return;
  size_t i = n, k = 0;
  while(i > 0 && k < 3 && ((uint8_t)s[i-1] & 0xC0) == 0x80){ i--; k++; }
  if(i == 0){ s[0] = 0; return; }
  uint8_t lead = (uint8_t)s[i-1];
  size_t need = (lead & 0x80) == 0 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 1;
  if(k + 1 < need) s[i-1] = 0;
}

/*  Рядкове поле об'єкта з розкриттям JSON-екранування. Сайт віддає
    кирилицю як є, але \" і \uXXXX теж обробляємо — на випадок лапок у назві. */
static bool jsonStr(const char* obj, const char* key, char* out, size_t cap){
  char pat[24]; snprintf(pat, sizeof(pat), "\"%s\":\"", key);
  const char* p = strstr(obj, pat);
  out[0] = '\0';
  if(!p) return false;
  p += strlen(pat);
  size_t o = 0;
  while(*p && *p != '"'){
    char c = *p++;
    if(c == '\\' && *p){
      char e = *p++;
      if(e == 'u' && isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1]) &&
                     isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3])){
        char hx[5] = { p[0], p[1], p[2], p[3], 0 }; p += 4;
        uint16_t u = (uint16_t)strtoul(hx, nullptr, 16);
        char enc[3]; uint8_t k = 0;
        if(u < 0x80) enc[k++] = (char)u;
        else if(u < 0x800){ enc[k++] = 0xC0 | (u >> 6); enc[k++] = 0x80 | (u & 0x3F); }
        else { enc[k++] = 0xE0 | (u >> 12); enc[k++] = 0x80 | ((u >> 6) & 0x3F); enc[k++] = 0x80 | (u & 0x3F); }
        for(uint8_t j = 0; j < k && o + 1 < cap; j++) out[o++] = enc[j];
        continue;
      }
      c = (e == 'n' || e == 't' || e == 'r') ? ' ' : e;
    }
    if(o + 1 < cap) out[o++] = c;
  }
  out[o] = '\0';
  utf8FixTail(out);                      /* обрізали посеред літери — прибираємо хвіст */
  return true;
}

static long jsonNum(const char* obj, const char* key){
  char pat[24]; snprintf(pat, sizeof(pat), "\"%s\":", key);
  const char* p = strstr(obj, pat);
  return p ? atol(p + strlen(pat)) : 0;
}

void YoSermons::fetch(){
  if(_loading) return;
  if(WiFi.status() != WL_CONNECTED){ _err = "нема мережі"; _ver++; return; }
  if(!_items){
    _items = (Sermon*)ps_malloc(sizeof(Sermon) * SERMON_MAX);
    if(!_items){ _err = "нема пам'яті"; _ver++; return; }
  }
  _loading = true;
  _got = 0;
  _err = "";
  if(xTaskCreatePinnedToCore(_task, "sermons", 12288, this, 1, nullptr, 0) != pdPASS){
    _loading = false; _err = "задача не створилась"; _ver++;
  }
}

void YoSermons::_task(void* arg){
  YoSermons* s = (YoSermons*)arg;
  char* obj = (char*)ps_malloc(SM_OBJ);
  uint16_t n = 0;
  const char* err = "";
  uint32_t t0 = millis();
  if(!obj){ err = "нема пам'яті"; goto done; }
  {
    WiFiClientSecure cli;
    cli.setInsecure();                   /* список публічний, перевірка сертифіката тут нічого не дає */
    HTTPClient http;
    http.setTimeout(8000);
    http.setConnectTimeout(8000);
    if(!http.begin(cli, SERMON_SITE "/api/sermons")){ err = "адреса сайту"; goto done; }
    int code = http.GET();
    /*  Не з'єднались — ще двічі, з паузою: мережа чи сайт могли на мить не відповісти  */
    for(uint8_t a = 0; a < 2 && code < 0; a++){
      Serial.printf("##SERM#\tсайт не відповів (%d), ще раз\n", code);
      http.end();
      vTaskDelay(pdMS_TO_TICKS(1500 * (a + 1)));
      if(!http.begin(cli, SERMON_SITE "/api/sermons")) break;
      code = http.GET();
    }
    if(code != 200){
      Serial.printf("##SERM#\tсайт відповів %d\n", code);
      err = code < 0 ? "сайт недоступний" : "сайт відповів помилкою";
      http.end();
      goto done;
    }
    WiFiClient* st = http.getStreamPtr();
    int depth = 0, ol = 0;
    bool inStr = false, esc = false, started = false, done = false;
    uint8_t chunk[1024];
    int total = http.getSize();            /* -1, якщо сайт віддає частинами */
    int got = 0;
    uint32_t deadline = millis() + 45000;
    /*  Кінець архіву — закрита остання дужка JSON (або вся довжина відповіді).
        Чекати, поки сайт закриє з'єднання, не можна: він тримає його відкритим,
        і радіо хвилину показувало «завантажую», хоч усе давно прийшло.  */
    while(!done && n < SERMON_MAX && (int32_t)(deadline - millis()) > 0){
      int av = st->available();
      if(!av){
        if(!st->connected()) break;
        vTaskDelay(pdMS_TO_TICKS(3));
        continue;
      }
      int r = st->read(chunk, av > (int)sizeof(chunk) ? sizeof(chunk) : av);
      if(r > 0) got += r;
      if(total > 0 && got >= total) done = true;
      for(int i = 0; i < r && n < SERMON_MAX; i++){
        char c = (char)chunk[i];
        bool keep = depth >= 2 || (c == '{' && depth == 1);
        if(inStr){
          if(esc) esc = false;
          else if(c == '\\') esc = true;
          else if(c == '"') inStr = false;
        }else if(c == '"') inStr = true;
        else if(c == '{'){ depth++; started = true; if(depth == 2) ol = 0; }
        else if(c == '}'){
          if(depth == 2){
            if(ol < SM_OBJ - 1) obj[ol++] = c;
            obj[ol] = '\0';
            keep = false;
            /*  Об'єкт цілий — беремо з нього потрібне.  */
            if(jsonNum(obj, "isReading") == 0){
              Sermon& it = s->_items[n];
              jsonStr(obj, "audioUrl", it.url, sizeof(it.url));
              if(it.url[0] == '/'){
                jsonStr(obj, "title", it.title, sizeof(it.title));
                jsonStr(obj, "preacher", it.preacher, sizeof(it.preacher));
                jsonStr(obj, "datePreached", it.date, sizeof(it.date));
                jsonStr(obj, "coverUrl", it.cover, sizeof(it.cover));
                it.dur = (uint16_t)jsonNum(obj, "audioDuration");
                n++;
                s->_got = n;                     /* лічильник для екрана, поки вантажиться */
              }
            }
          }
          depth--;
          if(started && depth == 0){ done = true; break; }   /* закрилась зовнішня дужка — усе */
        }
        if(keep && ol < SM_OBJ - 1) obj[ol++] = c;
      }
    }
    http.end();
  }
  if(n == 0 && !err[0]) err = "проповідей не знайдено";
done:
  if(obj) free(obj);
  Serial.printf("##SERM#\tпроповідей %u за %u мс %s\n", n, (unsigned)(millis() - t0), err);
  s->_n = n;
  s->_err = err;
  s->_ver++;
  s->_loading = false;
  vTaskDelete(nullptr);
}

bool YoSermons::play(uint16_t i){
  const Sermon* it = at(i);
  if(!it) return false;
  recorder.stop();
  if(config.getMode() == PM_SDCARD){
    config.changeMode(PM_WEB);
    /*  Перехід на радіо сам ставить у чергу «грати останню станцію». Та
        команда виконувалась першою, брала назву станції — і в шапці під
        час проповіді лишалась стара станція. Черги нам не треба.  */
    player.resetQueue();
  }
  snprintf(player.burl, sizeof(player.burl), "%s%s", SERMON_SITE, it->url);
  player.burlDur = it->dur;              /* для перемотки: секунда -> байт */
  player.burlSize = 0; player.burlBase = 0; player.burlResume = 0;
  player.burlRanged = false; player.yoRangeFrom = 0;
  strlcpy(player.burlTitle, it->preacher, sizeof(player.burlTitle));
  config.setStation(it->title);
  config.setTitle(it->preacher);
  display.putRequest(NEWSTATION);
  netserver.requestOnChange(STATION, 0);
  _playing = i;
  /*  Потік, що грає (радіо чи попередня проповідь), спершу зупиняємо: його
      з'єднання теж займає пам'ять, і третє — для обкладинки — не влазило.
      Чи грало радіо, запам'ятовуємо окремо, щоб після проповіді воно
      повернулось, як і раніше.  */
  if(!player.remoteStationName) player.burlResumeRadio = (player.status() == PLAYING) ? 1 : 0;
  if(player.status() == PLAYING) player.sendCommand({PR_STOP, 0});
  /*  Спершу обкладинка, потім звук: два з'єднання TLS одночасно не
      вміщаються в пам'ять — або звук не підключався, або обкладинка не
      вантажилась. Послідовно — обидва встигають (обкладинка ~1 с).  */
  if(_coverIdx == (int16_t)i || !it->cover[0]){ player.sendCommand({PR_BURL, 0}); }
  else{ _playAfterCover = true; _coverFetch(i); }
  return true;
}

bool YoSermons::playRel(int8_t d){
  int n = (int)_playing + d;
  if(_playing < 0 || n < 0 || n >= (int)count()) return false;
  return play((uint16_t)n);
}

/*  ---------- обкладинка ---------- */

/*  Сайт уміє віддати зменшену копію, але прогресивним JPEG, а декодер у
    ПЗУ ESP32-S3 (TJpgDec) знає лише звичайний. Тож беремо оригінал —
    1280x720, близько 40 КБ — і зменшуємо вже при розпакуванні: TJpgDec
    уміє ділити розмір на 2, 4 і 8, не рахуючи зайвого (1280 -> 160),
    а до 80x45 доводимо вже самі.  */
#define COVER_MAXB  (300 * 1024)

void YoSermons::_coverFetch(int16_t idx){
  _coverWant = idx;
  if(_coverBusy) return;                 /* задача підхопить нове бажання сама */
  _coverBusy = true;
  if(xTaskCreatePinnedToCore(_coverTask, "cover", 12288, this, 1, nullptr, 0) != pdPASS){
    _coverBusy = false;
    if(_playAfterCover){ _playAfterCover = false; player.sendCommand({PR_BURL, 0}); }   /* без обкладинки, але грати */
  }
}

struct CoverCtx { const uint8_t* p; size_t n, pos; uint8_t* rgb; uint16_t w, h; };

static UINT covIn(JDEC* jd, BYTE* buf, UINT nd){
  CoverCtx* c = (CoverCtx*)jd->device;
  if(c->pos + nd > c->n) nd = c->n - c->pos;
  if(buf) memcpy(buf, c->p + c->pos, nd);
  c->pos += nd;
  return nd;
}

static UINT covOut(JDEC* jd, void* bitmap, JRECT* r){
  CoverCtx* c = (CoverCtx*)jd->device;
  const uint8_t* src = (const uint8_t*)bitmap;
  for(uint16_t y = r->top; y <= r->bottom; y++){
    for(uint16_t x = r->left; x <= r->right; x++){
      if(x < c->w && y < c->h){ uint8_t* d = c->rgb + ((uint32_t)y * c->w + x) * 3; d[0] = src[0]; d[1] = src[1]; d[2] = src[2]; }
      src += 3;
    }
  }
  return 1;
}

/*  Розпакувати JPEG (звичайний, не прогресивний) і вписати в W x H, зі
    збереженням пропорцій і середнім по пікселях джерела. Спільне для
    обкладинок проповідей і логотипів станцій.  */
bool yoJpegFit(const uint8_t* jpg, size_t len, uint16_t* out, int W, int H){
  bool ok = false;
  CoverCtx c = { jpg, len, 0, nullptr, 0, 0 };
  JDEC jd;
  void* pool = malloc(4096);
  uint8_t* rgb = nullptr;
  if(pool && jd_prepare(&jd, covIn, pool, 4096, &c) == JDR_OK){
    uint8_t sc = 0;
    while(sc < 3 && (jd.width >> sc) > (unsigned)(W * 2)) sc++;
    c.w = (jd.width + (1 << sc) - 1) >> sc;
    c.h = (jd.height + (1 << sc) - 1) >> sc;
    rgb = (uint8_t*)ps_malloc((uint32_t)c.w * c.h * 3);
    c.rgb = rgb;
    if(rgb && jd_decomp(&jd, covOut, sc) == JDR_OK){
      float f = fminf((float)W / c.w, (float)H / c.h);
      int dw = (int)(c.w * f), dh = (int)(c.h * f);
      if(dw < 1) dw = 1; if(dh < 1) dh = 1;
      int ox = (W - dw) / 2, oy = (H - dh) / 2;
      for(int y = 0; y < H; y++)
        for(int x = 0; x < W; x++){
          uint16_t v = 0;
          int sx = x - ox, sy = y - oy;
          if(sx >= 0 && sy >= 0 && sx < dw && sy < dh){
            int x0 = sx * c.w / dw, x1 = (sx + 1) * c.w / dw; if(x1 <= x0) x1 = x0 + 1;
            int y0 = sy * c.h / dh, y1 = (sy + 1) * c.h / dh; if(y1 <= y0) y1 = y0 + 1;
            uint32_t r = 0, g = 0, b = 0, k = 0;
            for(int yy = y0; yy < y1; yy++)
              for(int xx = x0; xx < x1; xx++){
                const uint8_t* p = rgb + ((uint32_t)yy * c.w + xx) * 3;
                r += p[0]; g += p[1]; b += p[2]; k++;
              }
            r /= k; g /= k; b /= k;
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
          }
          out[y * W + x] = v;
        }
      ok = true;
    }
  }
  if(pool) free(pool);
  if(rgb) free(rgb);
  return ok;
}

void YoSermons::_coverTask(void* arg){
  YoSermons* s = (YoSermons*)arg;
  /*  чекаємо, поки звільниться пам'ять: зупинка потоку, пошук логотипа  */
  for(int i = 0; i < 30 && player.status() == PLAYING; i++) vTaskDelay(pdMS_TO_TICKS(100));
  /*  логотипи: прохід по станціях стане перед наступною (hold), поточний запит — дочекатись (до 30 с)  */
  logos.hold(true);
  for(int i = 0; i < 300 && logos.netBusy(); i++) vTaskDelay(pdMS_TO_TICKS(100));
  while(true){
    int16_t idx = s->_coverWant;
    const Sermon* it = s->at(idx);
    bool ok = false;
    uint8_t* jpg = nullptr; uint8_t* rgb = nullptr; void* pool = nullptr;   /* rgb і pool — у yoJpegFit */
    size_t len = 0;
    if(it && it->cover[0] == '/'){
      jpg = (uint8_t*)ps_malloc(COVER_MAXB);
      if(jpg){
        WiFiClientSecure cli; cli.setInsecure();
        HTTPClient http; http.setTimeout(4000); http.setConnectTimeout(4000);
        char url[160]; snprintf(url, sizeof(url), "%s%s", SERMON_SITE, it->cover);
        if(http.begin(cli, url) && http.GET() == 200){
          WiFiClient* st = http.getStreamPtr();
          int total = http.getSize();
          uint32_t deadline = millis() + 5000;
          while(len < COVER_MAXB && (int32_t)(deadline - millis()) > 0 && (total < 0 || (int)len < total)){
            int av = st->available();
            if(!av){ if(!st->connected()) break; vTaskDelay(pdMS_TO_TICKS(5)); continue; }
            len += st->read(jpg + len, (size_t)av > COVER_MAXB - len ? COVER_MAXB - len : av);
          }
        }
        http.end();
      }
    }
    if(len > 100){
      if(!s->_coverPix) s->_coverPix = (uint16_t*)ps_malloc(COVER_W * COVER_H * 2);
      if(s->_coverPix) ok = yoJpegFit(jpg, len, s->_coverPix, COVER_W, COVER_H);
      /*  і велика, для нового головного екрана  */
      s->_coverBigOk = false;
      if(ok){
        if(!s->_coverBig) s->_coverBig = (uint16_t*)ps_malloc(COVERB_W * COVERB_H * 2);
        if(s->_coverBig) s->_coverBigOk = yoJpegFit(jpg, len, s->_coverBig, COVERB_W, COVERB_H);
      }
    }
    if(pool) free(pool);
    if(rgb) free(rgb);
    if(jpg) free(jpg);
    Serial.printf("##SERM#\tобкладинка %d: %s, %u байт\n", idx, ok ? "готова" : "не вийшло", (unsigned)len);
    if(ok){ s->_coverIdx = idx; s->_coverVer++; }
    if(s->_coverWant == idx) break;      /* поки качали, не попросили іншу */
  }
  /*  обкладинка є (або не вийшло) — тепер звук  */
  if(s->_playAfterCover){ s->_playAfterCover = false; player.sendCommand({PR_BURL, 0}); }
  logos.hold(false);                     /* прохід по станціях — далі */
  s->_coverBusy = false;
  vTaskDelete(nullptr);
}
