#include "yoDebug.h"
#ifdef YO_DEBUG
volatile bool yoVolTrace = false;   /* voltrace: кроки анімації повзунка гучності (m2player.cpp) */

#include "../core/config.h"
#include "../core/commandhandler.h"
#include "../core/display.h"
#include "../core/player.h"
#include "../core/network.h"
#include "../core/timekeeper.h"
#include "../core/sdmanager.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "../displays/dspcore.h"
#include "../displays/tools/spidma.h"
#include "../m2/m2simd.h"
namespace m2 { extern volatile uint32_t yoRowDur, yoRowPos, yoRowN; }
#include "hal/spi_ll.h"
#include "esp_core_dump.h"
#include "nvs.h"
#include <EEPROM.h>
#include "driver/gpio.h"
#include "driver/i2s.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include "freertos_stats.h"
#include "esp_heap_caps.h"
#include <SPIFFS.h>
#include "../ES8311/yoES8311.h"
#include "yoMenu.h"
#include "../m2/m2pages.h"
#include "../m2/m2player.h"
#include "../extras/yoOta.h"
#include "../extras/yoVersion.h"
#include "../core/touchscreen.h"
#include "../extras/yoExtras.h"
#include "../extras/yoRecorder.h"
#include "../extras/yoSermons.h"
#include "../extras/yoLogos.h"
#include "../extras/yoMic.h"
#include "../extras/yoSfx.h"
#include "../extras/yoDsp.h"
#include "../extras/yoSpectrum.h"
#include "../extras/yoAirplay.h"
#include "../extras/yoHwTest.h"
#include "../extras/yoATrace.h"
#if TS_MODEL==TS_MODEL_ST77922
  #include "../ST77922_Touch/yoST77922Touch.h"
#endif

extern DspCore dsp;

static uint16_t vuSampleLeft = 0;
static bool vuWasOn = false;
static uint8_t vuMin = 255, vuMax = 0;
static uint8_t vuBarMin = 255, vuBarMax = 0;   /* те саме, але вже на екрані */

/*  Перевірка анімації гучності: знімаємо, де насправді стоїть смуга, кожні
    20 мс. Без затримок у циклі — інакше зупиниться звук.  */

/*  Приймання файлу в SPIFFS порядково, шістнадцятковим текстом. Потрібне,
    щоб оновити сторінки веб-інтерфейсу, не стираючи розділ цілком: поруч
    із ними в /data лежать збережена мережа й список станцій.  */
static File     putFile;
static uint32_t putBytes = 0;
static uint32_t vuTick = 0;

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64row(const uint8_t* d, size_t n){
  char out[4];
  for(size_t i=0;i<n;i+=3){
    uint32_t v = d[i] << 16 | (i+1<n ? d[i+1]<<8 : 0) | (i+2<n ? d[i+2] : 0);
    out[0]=B64[(v>>18)&63]; out[1]=B64[(v>>12)&63];
    out[2]=(i+1<n)?B64[(v>>6)&63]:'='; out[3]=(i+2<n)?B64[v&63]:'=';
    Serial.write((uint8_t*)out, 4);
  }
}

/*  Самоперевірка: малюємо смужку відомого кольору, читаємо назад і
    порівнюємо. Якщо не збіглося — знімкам вірити не можна.  */
#if DSP_MODEL==DSP_ST77922
/*  ES3C35P: відеопам'ять панелі читається лише стиснутою, тож знімок — із «тіні»
    (копії всього, що пішло в панель). Тінь вмикаємо, просимо перемалювати весь
    екран (меню або плеєр — що зараз відкрите) і даємо кадру дійти.  */
static void shadowFill(){
  const bool was = lcdShadowOn();
  lcdShadow(true);
  if(!was){
    if(yomenu.active()) m2::M.invalAll(); else m2::P.invalAll();
    delay(450);
  }
}
#endif

bool yodbgReadTest(){
  static uint16_t row[DSP_W];
  /*  Читання відеопам'яті забирає шину SPI і змінює її частоту, а задача
      дисплея малює виджети з іншого ядра. Без замикання це гонка за шину —
      саме вона лишає сміття на екрані.  */
  display.lock();
  delay(60);
  uint16_t probe = 0xF81F;                 /* яскраво-рожевий, у інтерфейсі не трапляється */
#if DSP_MODEL==DSP_ST77922
  lcdShadow(true);
#endif
  dsp.fillRect(0, 0, DSP_W, 8, probe);
  dsp.dbgReadRow(1, DSP_W, row);
  uint16_t got = row[10];
  bool ok = (got == probe);
  Serial.printf("SCRTEST: записано 0x%04X, прочитано 0x%04X -> %s\n", probe, got, ok?"ЧИТАННЯ ПРАЦЮЄ":"ЧИТАННЯ НЕ ПРАЦЮЄ");
  display.unlock();
  display.forceRedraw();      /* прибираємо смужку, якою перевіряли */
  return ok;
}

void yodbgScreenshot(){
  static uint16_t row[DSP_W];
#if DSP_MODEL==DSP_ST77922
  shadowFill();
#endif
  display.lock();             /* див. пояснення в yodbgReadTest() */
  delay(60);
  Serial.printf("SCR_BEGIN %d %d\n", DSP_W, DSP_H);
  for(uint16_t y=0; y<DSP_H; y++){
    dsp.dbgReadRow(y, DSP_W, row);
    b64row((uint8_t*)row, DSP_W * 2);
    Serial.println();
    if((y & 15)==15) delay(2);             /* даємо USB CDC віддихатись */
  }
  Serial.println("SCR_END");
  display.unlock();
}

/*  Знімок без паузи звуку — для консолі по Wi-Fi (scr.py --net): тут лише копія кадру,
    шле задача консолі (extras/yoNetCon). Без клієнта по Wi-Fi — звичайний «scr».  */
void yodbgScreenshotQuick(){
#if DSP_MODEL==DSP_ST77922
  shadowFill();
  display.lock();
  delay(30);                  /* задача екрана домальовує почате */
  const bool ok = yoNetConSnap(lcdFrame(), DSP_W, DSP_H);
  display.unlock();
  if(ok) return;
#endif
  yodbgScreenshot();
}

static void addStation(const char* name, const char* url){
  File f = SPIFFS.open(PLAYLIST_PATH, FILE_APPEND);
  if(!f){ Serial.println("DBG: не відкрився плейлист"); return; }
  f.printf("%s\t%s\t0\n", name, url);
  f.close();
}

void yodbgLoop(){
  /*  Вибірка рівня: один відлік за оберт головного циклу, без затримок.  */

  if(vuSampleLeft && millis() - vuTick >= 40){
    vuTick = millis();
    uint16_t v = player.get_VUlevel(100);
    uint8_t l = 100 - ((v>>8)&0xFF), r = 100 - (v&0xFF);
    if(l < vuMin) vuMin = l;
    if(l > vuMax) vuMax = l;
    extern uint16_t yoVuMeasL;
    uint8_t bar = (uint8_t)(yoVuMeasL > 100 ? 0 : 100 - yoVuMeasL);
    if(bar < vuBarMin) vuBarMin = bar;
    if(bar > vuBarMax) vuBarMax = bar;
    if((vuSampleLeft % 6) == 0) Serial.printf("  рівень L=%3d R=%3d\n", l, r);
    if(--vuSampleLeft == 0){
      Serial.printf("розмах лівого каналу за 2.4 с: %d..%d (розкид %d)\n", vuMin, vuMax, vuMax-vuMin);
      Serial.printf("  а на екрані, після згладжування: %d..%d (розкид %d)\n",
                    vuBarMin, vuBarMax, vuBarMax-vuBarMin);
      config.store.vumeter = vuWasOn;
    }
  }

  static char buf[160];
  static uint8_t pos = 0;
  while(Serial.available()){
    char c = Serial.read();
    if(c=='\r') continue;
    if(c!='\n'){ if(pos < sizeof(buf)-1) buf[pos++]=c; continue; }
    buf[pos]=0; pos=0;
    if(!buf[0]) continue;

    if(putFile){                       /* йде приймання файлу */
      if(!strcmp(buf,".")){
        putFile.close();
        Serial.printf("файл записано, %lu байт\n", (unsigned long)putBytes);
      }else{
        uint8_t chunk[96]; size_t n = strlen(buf)/2;
        if(n > sizeof(chunk)) n = sizeof(chunk);
        for(size_t i=0;i<n;i++){
          char hi = buf[i*2], lo = buf[i*2+1];
          uint8_t h = (uint8_t)(hi<='9' ? hi-'0' : (hi|32)-'a'+10);
          uint8_t l = (uint8_t)(lo<='9' ? lo-'0' : (lo|32)-'a'+10);
          chunk[i] = (uint8_t)((h<<4)|l);
        }
        putFile.write(chunk, n);
        putBytes += n;
      }
      continue;
    }

    if(!strncmp(buf,"hwtest",6))      yoHwTest(buf + 6);   /* самоперевірка всіх вузлів (extras/yoHwTest) */
    else if(!strcmp(buf,"heapmap")){
      /*  Хто тримає внутрішню пам'ять: зайняті шматки від 1 КБ (розмір і адреса)
          і підсумок. Спершу збираємо (обхід купи друкувати не можна — порт
          губить рядки), потім друкуємо з паузами.  */
      struct W { uint32_t used, free_, nUsed, largestFree; uint16_t n; uint32_t sz[64]; void* p[64]; };
      static W w; memset(&w, 0, sizeof(w));
      heap_caps_walk(MALLOC_CAP_INTERNAL, [](walker_heap_into_t hi, walker_block_info_t b, void* u) -> bool {
        W* w = (W*)u;
        if(b.used){ w->used += b.size; w->nUsed++; if(b.size >= 1024 && w->n < 64){ w->sz[w->n] = b.size; w->p[w->n] = b.ptr; w->n++; } }
        else { w->free_ += b.size; if(b.size > w->largestFree) w->largestFree = b.size;
               if(b.size >= 1024 && w->n < 64){ w->sz[w->n] = b.size | 0x80000000UL; w->p[w->n] = b.ptr; w->n++; } }
        return true;
      }, &w);
      for(uint16_t i = 0; i < w.n; i++){ Serial.printf("HEAPBLK %s %6u %p\n", (w.sz[i] & 0x80000000UL) ? "ВІЛЬНО " : "зайнято", (unsigned)(w.sz[i] & 0x7FFFFFFFUL), w.p[i]); Serial.flush(); delay(3); }
      Serial.printf("HEAPMAP внутрішня: зайнято %u Б у %u шматках (від 1 КБ — %u), вільно %u Б, найбільший вільний %u Б\n",
                    (unsigned)w.used, (unsigned)w.nUsed, (unsigned)w.n, (unsigned)w.free_, (unsigned)w.largestFree);
    }
#if TS_MODEL==TS_MODEL_ST77922
    else if(!strcmp(buf,"tsraw"))     { extern YoST77922Touch ts; ts.dbgRaw(); }
    else if(!strncmp(buf,"tsdark",6)){
      /*  Чи живий сенсор, коли екран погашено (0x28) і коли контролер спить (0x28 + 0x10):
          відповідає він по I2C і яка лінія INT. Потім екран вмикається знову.  */
      extern YoST77922Touch ts;
      const bool deep = buf[6] == ' ' && buf[7] == '1';
      display.lock(); delay(120);
      lcdPanelCmd(0x28); delay(20);
      if(deep){ lcdPanelCmd(0x10); delay(150); }
      const uint32_t e0 = ts.errors, r0 = ts.reports;
      for(uint8_t i = 0; i < 20; i++){ ts.read(); delay(50); }
      Serial.printf("TSDARK %s: опитів 20, помилок I2C +%u, звітів +%u, INT=%d\n", deep ? "0x28+0x10 (сон контролера)" : "0x28 (лише погашено)",
                    (unsigned)(ts.errors - e0), (unsigned)(ts.reports - r0), digitalRead(TS_INT));
      { uint8_t r[4] = { 0 }; const bool ok = ts.rdReg(0x0001, r, 3);
        Serial.printf("TSDARK регістри 0x01..0x03 (стан, керування, до простою): %02X %02X %02X%s\n", r[0], r[1], r[2], ok ? "" : " (помилка)"); }
      ts.dbgRaw();
      lcdPanelCmd(0x11); delay(130); lcdPanelCmd(0x29); delay(20);
      { uint8_t r[4] = { 0 }; ts.rdReg(0x0001, r, 3); Serial.printf("TSDARK після вмикання: %02X %02X %02X\n", r[0], r[1], r[2]); }
      display.unlock();
    }
    else if(!strcmp(buf,"tsreset"))   { extern YoST77922Touch ts; ts.hwReset(); Serial.println("TSRESET зроблено"); }
    else if(!strncmp(buf,"tsrel",5))  { extern YoST77922Touch ts; if(buf[5] == ' ') ts.releaseMs = (uint16_t)atoi(buf + 6); Serial.printf("TSREL %u мс\n", ts.releaseMs); }
    else if(!strcmp(buf,"tslog dump")) { extern YoST77922Touch ts; ts.logDump(); }
    else if(!strncmp(buf,"tslog",5))  { extern YoST77922Touch ts; ts.logStart(buf[5] == ' ' ? (uint16_t)atoi(buf + 6) : 10); }
#endif
    else if(!strcmp(buf,"scr"))       yodbgScreenshot();
    else if(!strcmp(buf,"scrq"))      yodbgScreenshotQuick();
    else if(!strcmp(buf,"atrace dump")) atrace::dump();
    else if(!strncmp(buf,"atrace",6) && (buf[6] == 0 || buf[6] == ' ')) atrace::arm(buf[6] == ' ' ? (uint32_t)atoi(buf + 7) : 6000);
#if DSP_MODEL==DSP_ST77922
    /*  ES3C35P: екран ST77922 — статистика виводу, поворот векторними інструкціями, тінь для знімків  */
    else if(!strncmp(buf,"lcdstat",7)) lcdStats(buf[7] == ' ' && buf[8] == 'r');
    else if(!strncmp(buf,"te",2) && (buf[2] == 0 || buf[2] == ' ')) lcdTe(buf[2] == ' ' ? (int8_t)atoi(buf + 3) : -1);
    else if(!strncmp(buf,"slidetest",9)) lcdSlideTest(buf[9] == ' ' ? (int8_t)atoi(buf + 10) : 1);
    else if(!strncmp(buf,"lcdrd ",6)){ unsigned r = 0, l = 1, d = 1; if(sscanf(buf + 6, "%x %u %u", &r, &l, &d) >= 1) lcdRegRead((uint8_t)r, (uint8_t)l, (uint8_t)d); }
    else if(!strncmp(buf,"rotpie",6))  lcdRotPie(buf[6] == ' ' ? (int8_t)atoi(buf + 7) : -1);
    else if(!strcmp(buf,"rotcheck"))   lcdRotCheck();
    else if(!strncmp(buf,"shadow ",7)){ lcdShadow(atoi(buf + 7) != 0); Serial.printf("SHADOW %d\n", lcdShadowOn() ? 1 : 0); }
#endif
    else if(!strcmp(buf,"scrtest"))   yodbgReadTest();
    else if(!strncmp(buf,"logobanner",10)){
      /*  Логотипи-банери серед уже знайдених: широка картинка, вписана по ширині, — однакові смуги
          згори й знизу, а з боків картинка торкається країв. «logobanner del» — стерти такі файли,
          щоб прохід логотипів («Завантажити всі логотипи») пошукав заново (з новою перевіркою банерів).  */
      const bool del = strstr(buf + 10, "del") != nullptr;
      File pl = SPIFFS.open(PLAYLIST_PATH, "r");
      uint16_t* px = (uint16_t*)heap_caps_malloc(LOGO_S * LOGO_S * 2, MALLOC_CAP_SPIRAM);
      uint16_t found = 0, n = 0;
      if(pl && px){
        char line[300];
        while(pl.available()){
          const size_t l = pl.readBytesUntil('\n', line, sizeof(line) - 1); line[l] = 0; n++;
          char* t1 = strchr(line, '\t'); if(!t1) continue; *t1 = 0;
          char* url = t1 + 1; char* t2 = strchr(url, '\t'); if(t2) *t2 = 0;
          char path[28]; snprintf(path, sizeof(path), "/logo/%08x.565", (unsigned)YoLogos::crc(url));
          File f = SPIFFS.open(path, "r");
          if(!f) continue;
          const bool ok = f.read((uint8_t*)px, LOGO_S * LOGO_S * 2) == LOGO_S * LOGO_S * 2; f.close();
          if(!ok) continue;
          auto near = [](uint16_t a, uint16_t b){ return abs((a >> 11) - (b >> 11)) <= 2 && abs(((a >> 5) & 63) - ((b >> 5) & 63)) <= 4 && abs((a & 31) - (b & 31)) <= 2; };
          const uint16_t c = px[0];
          auto rowU = [&](int y){ for(int x = 0; x < LOGO_S; x++) if(!near(px[y * LOGO_S + x], c)) return false; return true; };
          auto colU = [&](int x){ for(int y = 0; y < LOGO_S; y++) if(!near(px[y * LOGO_S + x], c)) return false; return true; };
          int t = 0, b = 0, lft = 0, rgt = 0;
          while(t < LOGO_S && rowU(t)) t++;
          while(b < LOGO_S && rowU(LOGO_S - 1 - b)) b++;
          while(lft < LOGO_S && colU(lft)) lft++;
          while(rgt < LOGO_S && colU(LOGO_S - 1 - rgt)) rgt++;
          if(t >= LOGO_S / 8 && b >= LOGO_S / 8 && lft <= 1 && rgt <= 1 && t < LOGO_S){
            found++;
            Serial.printf("LOGOBANNER %u «%s» смуги %d/%d, боки %d/%d %s\n", (unsigned)n, line, t, b, lft, rgt, del ? "— стерто" : "");
            if(del) SPIFFS.remove(path);
          }
        }
      }
      if(pl) pl.close();
      if(px) heap_caps_free(px);
      Serial.printf("LOGOBANNER станцій %u, банерів %u%s\n", (unsigned)n, (unsigned)found, del && found ? " — запустіть «logoall»" : "");
    }
    else if(!strncmp(buf,"logoqual",8)){
      /*  Чи прочитається кожен збережений логотип: розмір самого знака в клітинці (так само
          перевіряє пошук). «logoqual del» — стерти ті, що не прочитати (далі «logoall»).  */
      const bool del = strstr(buf + 8, "del") != nullptr;
      File pl = SPIFFS.open(PLAYLIST_PATH, "r");
      uint16_t* px = (uint16_t*)heap_caps_malloc(LOGO_S * LOGO_S * 2, MALLOC_CAP_SPIRAM);
      uint16_t bad = 0, have = 0, n = 0;
      if(pl && px){
        char line[300];
        while(pl.available()){
          const size_t l = pl.readBytesUntil('\n', line, sizeof(line) - 1); line[l] = 0; n++;
          char* t1 = strchr(line, '\t'); if(!t1) continue; *t1 = 0;
          char* url = t1 + 1; char* t2 = strchr(url, '\t'); if(t2) *t2 = 0;
          char path[28]; snprintf(path, sizeof(path), "/logo/%08x.565", (unsigned)YoLogos::crc(url));
          File f = SPIFFS.open(path, "r");
          if(!f){ Serial.printf("LOGOQUAL %u «%s» —%s\n", (unsigned)n, line, logos.isNo(YoLogos::crc(url)) ? " нема (позначка)" : " ще не шукали"); continue; }
          const bool ok = f.read((uint8_t*)px, LOGO_S * LOGO_S * 2) == LOGO_S * LOGO_S * 2; f.close();
          if(!ok) continue;
          have++;
          int bw, bh; yoLogoContent(px, bw, bh);
          const bool rd = yoLogoReadable(bw, bh);
          if(!rd){ bad++; if(del) SPIFFS.remove(path); }
          Serial.printf("LOGOQUAL %u «%s» знак %d×%d%s\n", (unsigned)n, line, bw, bh, rd ? "" : del ? " — НЕ ПРОЧИТАТИ, стерто" : " — НЕ ПРОЧИТАТИ");
        }
      }
      if(pl) pl.close();
      if(px) heap_caps_free(px);
      if(del && bad) logos.bump();
      Serial.printf("LOGOQUAL станцій %u, логотипів %u, не прочитати %u\n", (unsigned)n, (unsigned)have, (unsigned)bad);
    }
    else if(!strcmp(buf,"netcon"))    yoNetConInfo(Serial);     /* консоль по Wi-Fi: стан, клієнт, скільки надіслано */
    else if(!strncmp(buf,"netfreeze ",10)){
      /*  З'єднання станції «завмирає» (дані не беруться) на N с або до нового з'єднання —
          перевірка, як швидко радіо помічає мертвий потік і з'єднується наново.  */
      extern volatile uint32_t yoNetFreezeUntil; yoNetFreezeUntil = millis() + (uint32_t)atoi(buf + 10) * 1000UL;
      Serial.printf("NETFREEZE %d с\n", atoi(buf + 10));
    }
    else if(!strcmp(buf,"logowipe"))  { logos.wipeAll(); Serial.println("LOGOWIPE: усі логотипи — наново (у задачі проходу)"); }
    else if(!strcmp(buf,"logoall"))   { logos.fetchAllForce(); Serial.printf("LOGOALL станцій %u\n", (unsigned)logos.batchTotal()); }
    else if(!strncmp(buf,"logotest ",9)){ if(!logos.test(buf + 9)) Serial.println("LOGOTEST: зайнято або немає мережі"); }
    else if(!strncmp(buf,"pinact ",7)){
      /*  Чи «живе» вивід: читаємо рівень 20 000 разів поспіль (вхідний буфер вмикаємо
          й для виходу) — частка одиниць і скільки разів перемикався. Такти I2S дають
          тисячі перемикань, тиша — нуль.  */
      int g = atoi(buf + 7);
      if(g >= 0 && g < 49){
        gpio_input_enable((gpio_num_t)g);
        uint32_t hi = 0, tr = 0; int last = gpio_get_level((gpio_num_t)g);
        for(int i = 0; i < 20000; i++){ int v = gpio_get_level((gpio_num_t)g); hi += v; tr += (v != last); last = v; }
        Serial.printf("PINACT GPIO%d: одиниць %u%%, перемикань %u з 20000\n", g, (unsigned)(hi / 200), (unsigned)tr);
      }
    }
    else if(!strcmp(buf,"logost"))    Serial.printf("LOGOST іде %d: %u з %u, знайдено %u, зайнято %d, версія %u\n", logos.batchOn() ? 1 : 0,
                                                    (unsigned)logos.batchDone(), (unsigned)logos.batchTotal(), (unsigned)logos.batchFound(),
                                                    logos.busy() ? 1 : 0, (unsigned)logos.version());
    else if(!strcmp(buf,"stop"))      player.sendCommand({PR_STOP,0});
    else if(!strncmp(buf,"play ",5))  player.sendCommand({PR_PLAY,(uint16_t)atoi(buf+5)});
    else if(!strncmp(buf,"vol ",4))   player.sendCommand({PR_VOL,(uint16_t)atoi(buf+4)});
    else if(!strcmp(buf,"mode"))      config.changeMode();
    else if(!strncmp(buf,"add ",4)){
      char* tab = strchr(buf+4,'|');
      if(tab){ *tab=0; addStation(buf+4, tab+1); Serial.println("DBG: станцію додано"); }
    }
    else if(!strcmp(buf,"reindex")){ config.indexPlaylist(); Serial.println("DBG: індекс перебудовано"); }
    else if(!strncmp(buf,"sdls",4)){
      /*  Що лежить на картці: «sdls» — корінь, «sdls /Музика» — тека (до 200 записів).  */
      const char* path = buf[4] == ' ' ? buf + 5 : "/";
      if(!sdman.ready && !sdman.start()) Serial.println("SDLS: картки немає");
      else{
        File d = sdman.open(path);
        if(!d || !d.isDirectory()) Serial.printf("SDLS: «%s» — не тека\n", path);
        else{
          uint16_t n = 0; bool isDir = false;
          for(String f = d.getNextFileName(&isDir); f.length() && n < 200; f = d.getNextFileName(&isDir), n++){
            if(isDir) Serial.printf("  [тека] %s\n", f.c_str());
            else{ File x = sdman.open(f); Serial.printf("  %9u %s\n", x ? (unsigned)x.size() : 0, f.c_str()); if(x) x.close(); }
            Serial.flush();
          }
          Serial.printf("SDLS %s: %u записів\n", path, n);
        }
        if(d) d.close();
      }
    }
    else if(!strncmp(buf,"sdhead ",7)){
      /*  Перші рядки текстового файла на картці (плейлист картки — /data/playlistsd.csv).  */
      char path[120]; int lines = 20;
      if(sscanf(buf + 7, "%119s %d", path, &lines) >= 1){
        if(!sdman.ready) sdman.start();
        File f = sdman.open(path, "r");
        if(!f) Serial.printf("SDHEAD: немає «%s»\n", path);
        else{
          Serial.printf("SDHEAD %s, %u Б:\n", path, (unsigned)f.size());
          for(int i = 0; i < lines && f.available(); i++){ String l = f.readStringUntil('\n'); Serial.printf("  %s\n", l.c_str()); Serial.flush(); }
          f.close();
        }
      }
    }
    else if(!strncmp(buf,"sdhex ",6)){
      /*  Перші байти файла на картці шістнадцятково (заголовок WAV тощо): «sdhex /шлях [скільки]».  */
      char path[120]; int n = 96;
      if(sscanf(buf + 6, "%119s %d", path, &n) >= 1){
        if(n < 1) n = 1; if(n > 512) n = 512;
        if(!sdman.ready) sdman.start();
        File f = sdman.open(path, "r");
        if(!f) Serial.printf("SDHEX: немає «%s»\n", path);
        else{
          uint8_t b[512]; const int got = f.read(b, n); f.close();
          Serial.printf("SDHEX %s, перші %d Б:\n", path, got);
          for(int i = 0; i < got; i += 16){
            Serial.printf("  %04X ", i);
            for(int j = 0; j < 16 && i + j < got; j++) Serial.printf(" %02X", b[i + j]);
            Serial.print("  ");
            for(int j = 0; j < 16 && i + j < got; j++) Serial.print((b[i + j] >= 32 && b[i + j] < 127) ? (char)b[i + j] : '.');
            Serial.println(); Serial.flush();
          }
        }
      }
    }
    else if(!strncmp(buf,"sdspeed ",8)){
      /*  Швидкість читання картки: 2 МБ файла шматками по 32 КБ у PSRAM (як буфер потоку) і
          по 4 КБ у внутрішню пам'ять (її картка пише прямим доступом).  */
      char path[120];
      if(sscanf(buf + 8, "%119s", path) == 1){
        if(!sdman.ready) sdman.start();
        for(uint8_t mode = 0; mode < 2; mode++){
          File f = sdman.open(path, "r");
          if(!f){ Serial.printf("SDSPEED: немає «%s»\n", path); break; }
          const size_t chunk = mode ? 4096 : 32768;
          uint8_t* b = (uint8_t*)heap_caps_malloc(chunk, mode ? (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) : MALLOC_CAP_SPIRAM);
          if(!b){ f.close(); Serial.println("SDSPEED: немає пам'яті"); break; }
          const int64_t t0 = esp_timer_get_time(); size_t tot = 0;
          while(tot < 2 * 1024 * 1024){ const int r = f.read(b, chunk); if(r <= 0) break; tot += r; }
          const int64_t us = esp_timer_get_time() - t0;
          f.close(); heap_caps_free(b);
          Serial.printf("SDSPEED %s: %u КБ за %u мс — %.2f МБ/с\n", mode ? "внутрішня, по 4 КБ" : "PSRAM, по 32 КБ",
                        (unsigned)(tot / 1024), (unsigned)(us / 1000), us ? (double)tot / (double)us : 0.0);
        }
      }
    }
    else if(!strncmp(buf,"volanim ",8)){
      /*  Повзунок гучності «з'їжджає» з показаної позиції N до справжньої гучності — сама гучність
          не міняється. Разом із voltrace видно кожен крок руху.  */
      m2::P.volAnimFrom((float)atoi(buf + 8)); Serial.printf("VOLANIM з %d до %u\n", atoi(buf + 8), (unsigned)config.store.volume);
    }
    else if(!strncmp(buf,"gain",4) && (buf[4] == 0 || buf[4] == ' ')){
      /*  gain — скільки звук на виході DSP тихіший за вхід, дБ, з минулого виклику (і почати новий замір);
          gain 0 — вимкнути замір. Перевірка «максимальної гучності»: різниця двох замірів — саме обмеження.  */
      if(buf[4] == ' ' && atoi(buf + 5) == 0){ yoDsp.meter = false; Serial.println("GAIN вимкнено"); }
      else{
        if(yoDsp.meter && yoDsp.mN && yoDsp.mIn > 0)
          Serial.printf("GAIN %.2f дБ за %u відліків (гучність %u, обмеження %u %%)\n", 10.0f * log10f(yoDsp.mOut / yoDsp.mIn),
                        (unsigned)yoDsp.mN, (unsigned)config.store.volume, (unsigned)extras.s.volMax);
        else Serial.println("GAIN замір пішов");
        yoDsp.mIn = 0; yoDsp.mOut = 0; yoDsp.mN = 0; yoDsp.meter = true;
      }
    }
    else if(!strcmp(buf,"clk")){
      /*  Найбільший стрибок між сусідніми відліками на виході з минулого виклику (і скинути).
          Порівнювати: «clk» → секунда музики → «clk» (тло) → перемотка → «clk» (перехід).  */
      extern volatile uint32_t yoClickMax; Serial.printf("CLK найбільший стрибок %u\n", (unsigned)yoClickMax); yoClickMax = 0;
    }
    else if(!strncmp(buf,"seekfade ",9)){
      extern volatile bool yoSeekFade; yoSeekFade = atoi(buf + 9) != 0;
      Serial.printf("SEEKFADE %s\n", yoSeekFade ? "з затиханням" : "без затихання (як було)");
    }
    else if(!strcmp(buf,"voltrace")){
      /*  Друкувати кожен крок анімації повзунка гучності (VOLANIM) — перевірити, що він їде, а не стрибає.  */
      extern volatile bool yoVolTrace; yoVolTrace = !yoVolTrace;
      Serial.printf("VOLTRACE %s\n", yoVolTrace ? "увімкнено" : "вимкнено");
    }
    else if(!strcmp(buf,"tz")){
      /*  Пояс і літній час: що в TZ, котра година за UTC і місцева, чи діє літній час.  */
      const time_t t = time(nullptr); struct tm lt, ut; localtime_r(&t, &lt); gmtime_r(&t, &ut);
      char a[24], b[24]; strftime(a, sizeof(a), "%d.%m.%Y %H:%M:%S", &lt); strftime(b, sizeof(b), "%H:%M:%S", &ut);
      Serial.printf("TZ «%s»: місцевий %s (літній=%d), UTC %s; пояс %+d:%02d, літній час %s; на екрані %02d:%02d\n",
                    getenv("TZ") ? getenv("TZ") : "-", a, lt.tm_isdst, b, config.store.tzHour, abs(config.store.tzMin),
                    extras.s.dstMode == 1 ? "сам (Європа)" : "вимк", network.timeinfo.tm_hour, network.timeinfo.tm_min);
    }
    else if(!strcmp(buf,"vfs")){
      /*  Зареєстровані файлові системи й відкриті дескриптори (ESP-IDF) — шукати, хто тримає
          запис, коли картка не монтується вдруге («mount_to_vfs failed 0x101»).  */
      char* b = nullptr; size_t n = 0;
      FILE* f = open_memstream(&b, &n);
      if(f){ esp_vfs_dump_registered_paths(f); esp_vfs_dump_fds(f); fclose(f); }
      Serial.printf("VFS:\n%s\nVFS кінець\n", b ? b : "(нічого)");
      free(b);
    }
    else if(!strncmp(buf,"sdcycle",7)){
      /*  Відмонтувати й змонтувати картку N разів поспіль — як після «вийняли й вставили»:
          чи не кінчаються записи файлових систем (mount_to_vfs failed).  */
      int n = buf[7] == ' ' ? atoi(buf + 8) : 5; if(n < 1) n = 1; if(n > 50) n = 50;
      int ok = 0;
      for(int i = 0; i < n; i++){
        sdman.stop(); delay(50);
        const bool r = sdman.start();
        File d = r ? sdman.open("/") : File();
        const bool rd = d && d.isDirectory(); if(d) d.close();
        Serial.printf("SDCYCLE %d: змонтовано=%d, корінь читається=%d\n", i + 1, r ? 1 : 0, rd ? 1 : 0); Serial.flush();
        if(r && rd) ok++;
      }
      Serial.printf("SDCYCLE: %d з %d вдало\n", ok, n);
    }
    else if(!strcmp(buf,"sd")){
      Serial.printf("SD: ready=%d ", sdman.ready ? 1 : 0);
      if(!sdman.ready) Serial.printf("start=%d ", sdman.start() ? 1 : 0);
      if(sdman.ready) Serial.printf("тип=%d розмір=%lluМБ", (int)sdman.cardType(), sdman.cardSize()/(1024ULL*1024ULL));
      Serial.println();
    }
    else if(!strncmp(buf,"micgest ",8)){ mic.injectGesture((uint8_t)atoi(buf+8)); Serial.printf("MICGEST %d\n", atoi(buf+8)); }
    else if(!strcmp(buf,"info")){
      Serial.printf("остання перезавантаження: %s\n", YoExtras::resetReason());
      Serial.printf("режим=%s станція=%d гучність=%d грає=%d\n",
        config.getMode()==PM_SDCARD?"SD":"WEB", config.lastStation(),
        config.store.volume, player.isRunning()?1:0);
      Serial.printf("плейлист=%d станцій, ip=%s, heap=%u\n",
        config.playlistLength(), WiFi.localIP().toString().c_str(), (unsigned)ESP.getFreeHeap());
    }
    else if(!strcmp(buf,"net")){
      /*  Де саме рветься зв'язок: розбір імені чи саме з'єднання.  */
      Serial.printf("ip=%s шлюз=%s маска=%s dns=%s\n",
        WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(),
        WiFi.subnetMask().toString().c_str(), WiFi.dnsIP().toString().c_str());
      Serial.printf("RSSI=%d dBm канал=%d\n", (int)WiFi.RSSI(), (int)WiFi.channel());
      Serial.printf("PSRAM: всього=%u вільно=%u | heap=%u\n",
        (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram(), (unsigned)ESP.getFreeHeap());
      const char* hosts[] = { "stream.radioparadise.com", "ice1.somafm.com", "pool.ntp.org" };
      for(uint8_t i=0;i<3;i++){
        IPAddress a;
        uint32_t t0=millis();
        bool ok = WiFi.hostByName(hosts[i], a);
        Serial.printf("DNS %-26s %s (%s) %lums\n", hosts[i], ok?"OK":"ПОМИЛКА",
                      ok?a.toString().c_str():"-", (unsigned long)(millis()-t0));
        if(ok){
          WiFiClient c; c.setTimeout(5);
          t0=millis();
          bool con = c.connect(a, 80);
          Serial.printf("TCP %-26s %s %lums\n", hosts[i], con?"OK":"ПОМИЛКА", (unsigned long)(millis()-t0));
          c.stop();
        }
      }
    }
    else if(!strncmp(buf,"tcp ",4)){
      /*  Довільна адреса й порт: відрізнити «немає маршруту назовні»
          від «не працює сам клієнт».  */
      char* sp = strchr(buf+4,' ');
      if(sp){
        *sp = 0;
        IPAddress a; a.fromString(buf+4);
        WiFiClient c;
        uint32_t t0 = millis();
        bool ok = c.connect(a, (uint16_t)atoi(sp+1));
        Serial.printf("TCP %s:%d -> %s %lums\n", buf+4, atoi(sp+1), ok?"OK":"ПОМИЛКА", (unsigned long)(millis()-t0));
        c.stop();
      }
    }
    else if(!strncmp(buf,"esw ",4)){ unsigned r=0,v=0; sscanf(buf+4, "%x %x", &r, &v); Serial.printf("ES8311 reg%02X <- %02X: %s\n", r, v, es8311_write((uint8_t)r, (uint8_t)v) ? "OK" : "ПОМИЛКА"); }
    else if(!strcmp(buf,"es")){
      es8311_dump();
      /*  Читаємо не змінюючи режим виводу: перемикання в INPUT знімає
          рівень, який нога видає, і показує сміття.  */
      Serial.printf("вивід підсилювача GPIO%d = %s (низький = увімкнено), грає=%d\n",
                    MUTE_PIN, digitalRead(MUTE_PIN) ? "високий" : "низький",
                    player.isRunning()?1:0);
    }
    else if(!strcmp(buf,"vu")){
      /*  Не можна міряти рівень із затримками в цьому ж циклі: декодер звуку
          крутиться тут само, і delay() його зупиняє — значення завмирають.  */
      Serial.printf("частота=%u Гц біт=%d канали=%d бітрейт=%d\n",
        (unsigned)player.getSampleRate(), (int)player.getBitsPerSample(),
        (int)player.getChannels(), (int)player.getBitRate());
      vuSampleLeft = 60; vuWasOn = config.store.vumeter;
      config.store.vumeter = true; vuMin = 255; vuMax = 0;
      vuBarMin = 255; vuBarMax = 0;
    }
    else if(!strncmp(buf,"vuset",5)){
      /*  vuset <дБ> <спад> <підйом> <опускання> — підбір поведінки покажчика
          без перезбирання. Без аргументів просто показує поточні значення.  */
      extern uint8_t yoVuRange, yoVuRelease, yoVuRise, yoVuFade;
      int a=0,b=0,c=0,d=0;
      if(sscanf(buf+5, "%d %d %d %d", &a,&b,&c,&d)==4){
        if(a>=12 && a<=72)  yoVuRange   = (uint8_t)a;
        if(b>=2  && b<=8)   yoVuRelease = (uint8_t)b;
        if(c>=1  && c<=16)  yoVuRise    = (uint8_t)c;
        if(d>=1  && d<=16)  yoVuFade    = (uint8_t)d;
      }
      Serial.printf("покажчик: шкала=%d дБ спад=%d підйом=%d/16 опускання=%d/16\n",
                    yoVuRange, yoVuRelease, yoVuRise, yoVuFade);
    }
    else if(!strcmp(buf,"vufps")){
      /*  Скільки разів на секунду малюється покажчик і де стоїть стовпчик.
          Без цього плавність можна лише припускати.  */
      extern uint32_t yoVuCalls; extern uint16_t yoVuMeasL, yoVuMeasR;
      static uint32_t was = 0, wasms = 0;
      uint32_t now = millis(), dc = yoVuCalls - was, dt = now - wasms;
      Serial.printf("покажчик: %lu звернень за %lu мс (%lu на секунду), стовпчик L=%d R=%d з 100\n",
                    (unsigned long)dc, (unsigned long)dt,
                    dt ? (unsigned long)(dc*1000UL/dt) : 0UL,
                    (int)(100-yoVuMeasL), (int)(100-yoVuMeasR));
      was = yoVuCalls; wasms = now;
    }
    else if(!strncmp(buf,"put ",4)){
      /*  put <шлях> — далі рядки шістнадцяткою, крапка в окремому рядку
          завершує. Стирається тільки цей файл, решта розділу ціла.  */
      putFile = SPIFFS.open(buf+4, FILE_WRITE);
      putBytes = 0;
      Serial.printf("приймаю %s: %s\n", buf+4, putFile ? "готовий" : "НЕ ВІДКРИВСЯ");
    }
    else if(!strncmp(buf,"ls",2)){
      File d = SPIFFS.open(strlen(buf)>3 ? buf+3 : "/www");
      if(d && d.isDirectory()){
        File f = d.openNextFile();
        while(f){ Serial.printf("  %s %u\n", f.name(), (unsigned)f.size()); f = d.openNextFile(); }
      }else Serial.println("теки немає");
      Serial.printf("SPIFFS: зайнято %u з %u байт\n",
                    (unsigned)SPIFFS.usedBytes(), (unsigned)SPIFFS.totalBytes());
    }
    else if(!strcmp(buf,"weather")){
      Serial.printf("погода: показ=%d ключ=%s широта=%s довгота=%s\n",
                    config.store.showweather ? 1 : 0,
                    strlen(config.store.weatherkey) ? "є" : "НЕМАЄ",
                    config.store.weatherlat, config.store.weatherlon);
      Serial.printf("  рядок: \"%s\"\n", timekeeper.weatherBuf ? timekeeper.weatherBuf : "(не виділено)");
      Serial.printf("  значок за умовою: %d\n", (int)timekeeper.weatherIcon);
    }
    else if(!strcmp(buf,"sdindex")){
      /*  Пересобрати плейлист картки: після зміни правил відбору файлів
          старий індекс на картці лишається, доки його не перебудувати.  */
      if(config.getMode() != PM_SDCARD) Serial.println("sdindex: спершу перемкніть на картку");
      else{ sdman.indexSDPlaylist(); config.initPlaylistMode(); Serial.printf("sdindex: треків %u\n", (unsigned)config.playlistLength()); }
    }
    else if(!strncmp(buf,"wifidrop",8)){
      /*  Обрив зв'язку на замовлення: так перевіряють, як радіо повертається
          в мережу й чи лишається живим екран і звук. З числом — скільки
          секунд не намагатися назад (щоб устигнути подивитись на екран).  */
      int hold = atoi(buf + 8);
      Serial.printf("розриваю зв'язок з мережею, тримаю %d с\n", hold);
      WiFi.disconnect();
      if(hold > 0){
        network.pauseSta(true);
        timekeeper.waitAndDo((uint8_t)(hold > 250 ? 250 : hold), [](){ network.pauseSta(false); });
      }
    }
    else if(!strcmp(buf,"mic")){
      Serial.printf("мікрофон: слухає=%d рівень=%d дБ фон=%d дБ голос=%d блоків=%u луна=%d режим=%u кадр=%d віднято=%.1f дБ удар=%.2f слоти L=%d R=%d почуто=%s кімната=+%.1f зв'язок=%.1f\n",
        mic.listening()?1:0, (int)mic.levelDb(), (int)mic.noiseDb(), mic.speech()?1:0,
        (unsigned)mic.blocks(), mic.aecActive()?1:0, (unsigned)mic.aecMode(), mic.aecChunk(), mic.echoCut(), mic.onsetVal(),
        (int)mic.slotDb(0), (int)mic.slotDb(1), YoMic::gestureName(mic.heard()), mic.excessDb(), mic.coupleDb());
    }
    else if(!strncmp(buf,"micgain ",8)){
      extras.s.micGain = (uint8_t)atoi(buf+8); extras.changed(); mic.apply();
      Serial.printf("підсилення мікрофона: крок %u\n", (unsigned)extras.s.micGain);
    }
    else if(!strncmp(buf,"micset ",7)){
      /*  micset <хлопки 0/1> <стук 0/1> <під час звуку 0/1>  */
      int c=0,k=0,p=0; sscanf(buf+7, "%d %d %d", &c, &k, &p);
      extras.s.clapOn = c; extras.s.knockOn = k; extras.s.micPlay = p; extras.changed();
      Serial.printf("хлопки=%d стук=%d під час звуку=%d\n", c, k, p);
    }
    else if(!strncmp(buf,"mictest",7)){
      /*  mictest [рівень тону, дБ; типово -20] [noamp — підсилювач вимкнений] [quick — 5 частот]  */
      int lv = buf[7] == ' ' ? atoi(buf+8) : -20;
      if(!lv) lv = -20;
      bool amp = !strstr(buf, "noamp");
      uint32_t mask = strstr(buf, "quick") ? (1UL<<3 | 1UL<<9 | 1UL<<12 | 1UL<<17 | 1UL<<21) : 0;
      const char* why = mic.sweepStart((int8_t)lv, amp, mask, strstr(buf, " dsp") != nullptr);
      if(why) Serial.printf("самоперевірка звуку: не почато — %s\n", why);
      else    Serial.printf("самоперевірка звуку: почато, тон %d дБ, частота тактів %u Гц\n", lv, (unsigned)player.getSampleRate());
    }
    else if(!strcmp(buf,"eq")){
      const ExtStore& e = extras.s;
      Serial.printf("EQ on=%u preset=%u(%s) loud=%u guard=%u vbass=%u room=%u pre=%.1f us=%.2f limit=%u vol=%u rate=%u\n",
        e.eqOn, e.eqPreset, YoDsp::PRESET_NAME[e.eqPreset < EQ_PRESETS ? e.eqPreset : 0], e.eqLoud, e.eqGuard, e.vbass, e.eqRoomOn,
        yoDsp.preampDb(), yoDsp.usPerFrame(), (unsigned)yoDsp.limitHits(), (unsigned)player.getVolume(), (unsigned)player.getSampleRate());
      for(uint8_t b = 0; b < EQ_BANDS; b++)
        Serial.printf("EQB %u %d %d %.1f\n", YoDsp::BAND_HZ[b], e.eq[b], e.eqRoom[b], yoDsp.responseDb(YoDsp::BAND_HZ[b]));
    }
    else if(!strncmp(buf,"eqp ",4)){ yoDsp.applyPreset((uint8_t)atoi(buf+4)); Serial.printf("пресет %u\n", extras.s.eqPreset); }
    else if(!strncmp(buf,"eqb ",4)){ int b=0,d=0; sscanf(buf+4, "%d %d", &b, &d); yoDsp.setBand((uint8_t)b, (int8_t)d); Serial.printf("смуга %d = %d\n", b, d); }
    else if(!strncmp(buf,"eqo ",4)){
      /*  eqo <еквалайзер 0/1> <тонкомпенсація 0..2> <захист 0..2> <віртуальний бас 0..3> <кімната 0/1>  */
      int on=1, ld=0, g=1, vb=0, rm=0; sscanf(buf+4, "%d %d %d %d %d", &on, &ld, &g, &vb, &rm);
      extras.s.eqOn = on; extras.s.eqLoud = ld; extras.s.eqGuard = g; extras.s.vbass = vb; extras.s.eqRoomOn = rm;
      extras.changed(); yoDsp.changed();
      Serial.printf("звук: eq=%d тонкомп=%d захист=%d бас=%d кімната=%d\n", on, ld, g, vb, rm);
    }
    else if(!strcmp(buf,"dspbench")){
      if(player.isRunning()) Serial.println("DSPBENCH спершу зупиніть звук");
      else {
        float us = yoDsp.bench(44100);
        Serial.printf("DSPBENCH us=%.3f ланок=%u частка ядра=%.1f%%\n", us, (unsigned)yoDsp.stages(), us * 44100.0f / 1e4f);
      }
    }
    else if(!strcmp(buf,"eqmigrate")){
      /*  повторити перенесення трьох старих повзунків (після виправлення меж)  */
      yoDsp.fromTone(config.store.bass, config.store.middle, config.store.trebble);
      Serial.printf("перенесено: низькі %d середні %d високі %d\n", config.store.bass, config.store.middle, config.store.trebble);
    }
    else if(!strcmp(buf,"eqroom")){ char w[48]; bool ok = yoDsp.roomFromSweep(w, sizeof(w)); Serial.printf("кімната: %s %s\n", ok ? "OK" : "ні", w); }
    else if(!strcmp(buf,"roomtune")){ const char* w = yoDsp.roomTuneStart(); Serial.printf("ROOM %s\n", w ? w : "почато"); }
    else if(!strcmp(buf,"roomst")){ Serial.printf("ROOM state=%u progress=%u msg=%s\n", yoDsp.roomState(), yoDsp.roomProgress(), yoDsp.roomMsg()); }
    else if(!strcmp(buf,"eqroomclr")){ yoDsp.roomClear(); Serial.println("кімната: поправку скинуто"); }
    else if(!strcmp(buf,"eqresp")){
      /*  розрахункова АЧХ на частотах заміру — для tools/selftest.py  */
      for(uint8_t i = 0; i < YoMic::SWEEP_N; i++) Serial.printf("RESP %u %.2f\n", (unsigned)YoMic::sweepHz(i), yoDsp.responseDb(YoMic::sweepHz(i)));
      Serial.println("RESP end");
    }
    else if(!strncmp(buf,"aecmode ",8)){ int m=0,l=4; sscanf(buf+8, "%d %d", &m, &l); mic.aecSet((uint8_t)m, (uint8_t)l); Serial.printf("AEC режим %d, фільтр %d\n", m, l); }
    else if(!strncmp(buf,"micdbg ",7)){ mic._dbg = atoi(buf+7) != 0; Serial.printf("удари: %s\n", mic._dbg ? "друкую" : "мовчу"); }
    else if(!strncmp(buf,"micfast ",8)){ mic.minMs = atoi(buf+8) ? 1000 : 60000; Serial.printf("хвилина мікрофона = %u мс\n", (unsigned)mic.minMs); }
    else if(!strcmp(buf,"pres")){
      uint32_t now = millis();
      Serial.printf("PRES темно=%d пригашено=%d голос=%d тиша=%u с дотик=%u с сон=%u с ear=%u wake=%u off=%u\n",
        extras.presenceDark()?1:0, extras.screenDim()?1:0, mic.speech()?1:0,
        (unsigned)(mic.lastRoomMs() ? (now - mic.lastRoomMs()) / 1000 : 9999), (unsigned)((now - extras.lastTouchMs()) / 1000),
        (unsigned)extras.sleepLeftSec(), extras.s.sleepEar, extras.s.presWake, extras.s.presOff);
    }
    else if(!strncmp(buf,"micsim ",7)){
      /*  micsim <clap|knock> <скільки> <крок, мс>  */
      char kind[8] = {0}; int n = 2, gap = 300;
      sscanf(buf+7, "%7s %d %d", kind, &n, &gap);
      const char* w = mic.simStart(!strcmp(kind, "voice") ? 2 : !strcmp(kind, "knock") ? 1 : 0, (uint8_t)n, (uint16_t)gap);
      Serial.printf("MICSIM %s\n", w ? w : "почато");
    }
    else if(!strncmp(buf,"micext ",7)){
      /*  micext <clap|knock> <скільки> <крок, мс> <пік, дБ> — удар «із кімнати» в сигнал мікрофона  */
      char kind[8] = {0}; int n = 2, gap = 300, pk = -20;
      sscanf(buf+7, "%7s %d %d %d", kind, &n, &gap, &pk);
      const char* w = mic.extStart(!strcmp(kind, "knock") ? 1 : 0, (uint8_t)n, (uint16_t)gap, (int8_t)pk);
      Serial.printf("MICEXT %s\n", w ? w : "почато");
    }
    else if(!strcmp(buf,"micstop")){ mic.sweepAbort(); Serial.println("самоперевірка звуку: зупиняю"); }
    else if(!strcmp(buf,"micres")){
      /*  Машинний формат — його читає tools/selftest.py:
          SWEEP state=<0..3> pos=<n> of=<N> rate=<Гц> level=<дБ>
          SW <Гц> <тон дБ> <фон дБ> <2-га гарм. дБ> <3-тя гарм. дБ>  */
      Serial.printf("SWEEP state=%u pos=%u of=%u rate=%u level=%d amp=%d dsp=%d\n", (unsigned)mic.sweepState(), (unsigned)mic.sweepPos(),
        (unsigned)YoMic::SWEEP_N, (unsigned)mic.sweepRate(), (int)mic.sweepLevel(), mic.sweepAmp()?1:0, mic.sweepDsp()?1:0);
      if(mic.sweepState() >= 2){
        for(uint8_t i = 0; i < mic.sweepPos() && i < YoMic::SWEEP_N; i++)
          Serial.printf("SW %u %.1f %.1f %.1f %.1f\n", (unsigned)YoMic::sweepHz(i), mic.sweepDb(i), mic.sweepNoise(i), mic.sweepH2(i), mic.sweepH3(i));
      }
      Serial.println("SWEEP end");
    }
    else if(!strcmp(buf,"heap")){
      Serial.printf("HEAP internal=%u blk=%u min=%u psram=%u psblk=%u\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    }
    else if(!strncmp(buf,"micon ",6)){
      extras.s.micOn = atoi(buf+6) ? 1 : 0; extras.changed(); mic.apply();
      Serial.printf("мікрофон %s\n", extras.s.micOn ? "увімкнено" : "вимкнено");
    }
    else if(!strncmp(buf,"airplay click",13)){ extern volatile bool yoApClick; yoApClick = buf[13] != ' ' || buf[14] != '0'; Serial.printf("AIRPLAY CLICK %d\n", (int)yoApClick); }
    else if(!strcmp(buf,"airplay")){
      airplay.stat();
    }
    else if(!strcmp(buf,"tasks")){
      /*  Хто скільки процесора з'їв: список задач FreeRTOS від самого ядра.  */
      printRunningTasks(Serial);
    }
    else if(!strcmp(buf,"wscan")){
      /*  Що бачить радіо: назва, сигнал, канал, захист, BSSID. 5 ГГц ESP32-S3 не бачить зовсім.  */
      network.pauseSta(true);
      int16_t n = WiFi.scanNetworks(false, true);
      static const char* AUTH[] = { "відкрита", "WEP", "WPA", "WPA2", "WPA/WPA2", "WPA2-Ent", "WPA3", "WPA2/WPA3", "WAPI", "OWE", "WPA3-Ent", "WPA3-Ent192" };
      for(int16_t i = 0; i < n; i++){
        uint8_t a = WiFi.encryptionType(i);
        Serial.printf("WSCAN %-32s %4d dBm  кан %2d  %-10s %s\n", WiFi.SSID(i).c_str(), (int)WiFi.RSSI(i), (int)WiFi.channel(i),
                      a < 12 ? AUTH[a] : "?", WiFi.BSSIDstr(i).c_str());
      }
      Serial.printf("WSCAN end %d\n", (int)n);
      WiFi.scanDelete();
      network.pauseSta(false);
    }
    else if(!strncmp(buf,"wjoin ",6)){
      /*  wjoin <назва>|<пароль> — та сама спроба, що й з меню  */
      char* bar = strchr(buf+6, '|');
      if(bar){ *bar = 0; network.connectTo(buf+6, bar+1); Serial.printf("WJOIN %s\n", buf+6); }
    }
    else if(!strcmp(buf,"wlist")){
      /*  Збережені мережі: назва й довжина пароля (самого пароля не друкуємо).  */
      for(uint8_t i = 0; i < config.ssidsCount; i++)
        Serial.printf("WLIST %u%s %s  пароль %u симв.\n", (unsigned)(i + 1), config.store.lastSSID == i + 1 ? "*" : " ",
                      config.ssids[i].ssid, (unsigned)strlen(config.ssids[i].password));
      File f = SPIFFS.open(SSIDS_PATH, "r");
      Serial.printf("WLIST end %u, файл %u байт\n", (unsigned)config.ssidsCount, f ? (unsigned)f.size() : 0);
      if(f) f.close();
    }
    else if(!strcmp(buf,"wstate")) network.dump();
    else if(!strcmp(buf,"wps")){
      /*  Енергоощадження Wi-Fi насправді (не те, що просили): у сні модему радіо приймає
          пакети лише на маяках точки — +100 мс до кожного обміну, а вікно TCP тут 5760 Б.  */
      wifi_ps_type_t ps = WIFI_PS_NONE; esp_wifi_get_ps(&ps);
      wifi_bandwidth_t bw = WIFI_BW_HT20; esp_wifi_get_bandwidth(WIFI_IF_STA, &bw);
      uint8_t prot = 0; esp_wifi_get_protocol(WIFI_IF_STA, &prot);
      Serial.printf("WPS енергоощадження %s, смуга %s, протоколи 0x%02X, RSSI %d\n",
                    ps == WIFI_PS_NONE ? "вимкнено" : ps == WIFI_PS_MIN_MODEM ? "MIN_MODEM (сон модему)" : "MAX_MODEM",
                    bw == WIFI_BW_HT40 ? "40 МГц" : "20 МГц", (unsigned)prot, (int)WiFi.RSSI());
    }
    else if(!strncmp(buf,"sfx ",4)){
      /*  sfx <подія або номер> — програти (навіть вимкнену)  */
      int e = YoSfx::find(buf + 4);
      if(e >= 0){ sfx.test((SfxEvent)e); Serial.printf("SFX %s (%u мс), підсилення %.2f дБ, обмеження %u %%\n", YoSfx::id((SfxEvent)e), (unsigned)sfx.clipMs((SfxEvent)e), YoSfx::gainDb((SfxEvent)e), (unsigned)extras.s.volMax); }
      else Serial.println("SFX невідома подія");
    }
    else if(!strncmp(buf,"splashdemo",10)){ uint32_t ms = buf[10] == ' ' ? atoi(buf + 11) : 8000; display.splashDemo(ms); Serial.printf("SPLASH %u мс\n", (unsigned)ms); }
    else if(!strcmp(buf,"sfxls")){
      Serial.printf("SFX розділ: %s, %u з %u байт\n", sfx.fsOk() ? "є" : "немає", (unsigned)sfx.fsUsed(), (unsigned)sfx.fsTotal());
      for(uint8_t i = 0; i < SFX_N; i++)
        Serial.printf("SFX %u %-8s %s %u мс%s\n", i, YoSfx::id((SfxEvent)i), (extras.s.sfxMask >> i) & 1 ? "увімк" : "вимк ",
                      (unsigned)sfx.clipMs((SfxEvent)i), (sfx.userMask() >> i) & 1 ? " (свій)" : "");
    }
    else if(!strcmp(buf,"wnonet")){
      Serial.println("WNONET перезавантажуюсь, наче мережі поруч немає");
      MyNetwork::skipBootWifi();
      delay(300);
      ESP.restart();
    }
    else if(!strncmp(buf,"wstatic ",8)){
      /*  wstatic <адреса> <шлюз> — підключитись до першої збереженої мережі без DHCP (адреса
          вручну, маска /24, DNS = шлюз): перевірка, чи не в DHCP мережі річ, коли радіо
          з'єднується з точкою, а адреси не дістає. Після перезапуску — знову DHCP.  */
      char a[20] = {0}, g[20] = {0};
      IPAddress ip, gw;
      if(sscanf(buf + 8, "%19s %19s", a, g) == 2 && ip.fromString(a) && gw.fromString(g) && config.ssidsCount){
        WiFi.config(ip, gw, IPAddress(255, 255, 255, 0), gw);
        network.connectTo(config.ssids[0].ssid, config.ssids[0].password);
        Serial.printf("WSTATIC %s через %s до «%s»\n", a, g, config.ssids[0].ssid);
      }else Serial.println("WSTATIC: wstatic <адреса> <шлюз>");
    }
    else if(!strcmp(buf,"wdhcp")){
      /*  Підключитись до першої збереженої мережі з DHCP і чекати адресу без обмеження
          (спробу тримаємо — коло повторів її не перебиває): скільки насправді відповідає DHCP.  */
      if(config.ssidsCount){
        WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
        network.tryHeld = true;
        network.connectTo(config.ssids[0].ssid, config.ssids[0].password);
        Serial.printf("WDHCP %lu мс: чекаю адресу від DHCP\n", (unsigned long)millis());
      }
    }
    else if(!strcmp(buf,"wtry")){
      Serial.printf("WTRY стан=%d status()=%d ssid='%s' ip=%s\n", (int)network.tryState(), (int)WiFi.status(), WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    }
    else if(!strcmp(buf,"wifioff")){
      /*  Погасити радіомодуль зовсім — перевірити, чи це він душить екран.  */
      WiFi.scanDelete();
      WiFi.mode(WIFI_OFF);
      Serial.println("радіомодуль вимкнено");
    }
    else if(!strcmp(buf,"wifikeep")){
      /*  Записати мережу, у якій радіо зараз, у список — назву й пароль
          бере в самого радіомодуля, тож набирати нічого не треба.  */
      wifi_config_t c;
      if(esp_wifi_get_config(WIFI_IF_STA, &c) != ESP_OK || !c.sta.ssid[0]){
        Serial.println("радіомодуль не пам'ятає жодної мережі");
      }else{
        /*  Додаємо першою, а решту збережених лишаємо. Раніше команда
            записувала лише цю одну — і стерла власникові домашню мережу.  */
        String out = String((const char*)c.sta.ssid) + "\t" + String((const char*)c.sta.password) + "\n";
        uint8_t n = 1;
        for(uint8_t i = 0; i < config.ssidsCount && n < 5; i++){
          if(!strcmp(config.ssids[i].ssid, (const char*)c.sta.ssid)) continue;
          out += String(config.ssids[i].ssid) + "\t" + String(config.ssids[i].password) + "\n";
          n++;
        }
        config.saveWifiList(out.c_str());
        config.setLastSSID(1);
        Serial.printf("мережу %s збережено першою, усього в списку %u\n", (const char*)c.sta.ssid, (unsigned)config.ssidsCount);
      }
    }
    else if(!strcmp(buf,"page")){
      extern char yoSlowWhat[40]; extern uint32_t yoSlowMs;
      extern uint32_t yoLoopN, yoLoopMax, yoLoopFrom;
      uint32_t secs = (millis() - yoLoopFrom) / 1000;
      Serial.printf("цикл: %u обертів за %u с (%u/с), найдовший проміжок %u мс\n",
                    (unsigned)yoLoopN, (unsigned)secs, (unsigned)(secs ? yoLoopN/secs : 0), (unsigned)yoLoopMax);
      yoLoopN = 0; yoLoopMax = 0; yoLoopFrom = millis();
      extern uint32_t yoDspN, yoDspMax, yoDspFrom, yoDspDraw, yoDspNet;
      uint32_t ds = (millis() - yoDspFrom) / 1000;
      Serial.printf("екран: %u кадрів за %u с (%u/с), найдовший проміжок %u мс; малювання %u мс, веб %u мс\n",
                    (unsigned)yoDspN, (unsigned)ds, (unsigned)(ds ? yoDspN/ds : 0),
                    (unsigned)yoDspMax, (unsigned)yoDspDraw, (unsigned)yoDspNet);
      extern uint32_t yoMenuMs, yoFadeMs, yoDspWhatMs; extern char yoDspWhat[24];
      Serial.printf("  з них: меню %u мс, наплив %u мс, плеєр: %s %u мс\n",
                    (unsigned)yoMenuMs, (unsigned)yoFadeMs,
                    yoDspWhat[0] ? yoDspWhat : "-", (unsigned)yoDspWhatMs);
      extern uint32_t spidmaBytes;
      Serial.printf("  у екран через DMA: %u КБ за %u с\n", (unsigned)(spidmaBytes / 1024), (unsigned)ds);
      spidmaBytes = 0;
      yoMenuMs = 0; yoFadeMs = 0; yoDspWhatMs = 0; yoDspWhat[0] = 0;
      yoDspN = 0; yoDspMax = 0; yoDspDraw = 0; yoDspNet = 0; yoDspFrom = millis();
      if(yoSlowMs) Serial.printf("найдовший крок циклу: %s %u мс\n", yoSlowWhat, (unsigned)yoSlowMs);
      else         Serial.println("довгих кроків циклу не було");
      yoSlowMs = 0; yoSlowWhat[0] = 0;
      Serial.printf("меню відкрите=%d | режим екрана=%d\n", yomenu.active()?1:0, (int)display.mode());
      Serial.printf("пошук: іде=%d знайдено=%u scanComplete=%d режим=%d\n",
        yomenu.scanning()?1:0, (unsigned)yomenu.scanCount(), (int)WiFi.scanComplete(), (int)WiFi.getMode());
      Serial.printf("мережа: статус=%d втрачено=%d спроби спинено=%d спроб=%u status()=%d ssid='%s'\n",
        (int)network.status, network.linkLost?1:0, network.staPaused?1:0,
        (unsigned)network.lostTries(), (int)WiFi.status(), WiFi.SSID().c_str());
    }
    else if(!strcmp(buf,"menu"))   yomenu.open();
    else if(!strcmp(buf,"mwifi"))  yomenu.openWifi(false);   /* без замка: це перевірка, а не режим точки доступу */
    else if(!strcmp(buf,"mclose")) yomenu.close();
    else if(!strncmp(buf,"touchlog ",9)){ extern volatile bool yoTouchLog; yoTouchLog = atoi(buf+9) != 0; Serial.printf("TOUCHLOG %d\n", (int)yoTouchLog); }
    else if(!strcmp(buf,"spos")){
      /*  сирі числа позиції: щоб бачити, яке саме з них бреше  */
      Serial.printf("SPOS екран бачить: довжина %lu, позиція %lu (перемальовок рядка %lu)\n",
                    (unsigned long)m2::yoRowDur, (unsigned long)m2::yoRowPos, (unsigned long)m2::yoRowN);
      Serial.printf("SPOS файл %lu/%lu, у буфері %lu, час %lu/%lu, sd_min %lu sd_max %lu, показуємо %lu\n",
                    (unsigned long)player.getFilePos(), (unsigned long)player.getFileSize(),
                    (unsigned long)player.inBufferFilled(),
                    (unsigned long)player.getAudioCurrentTime(), (unsigned long)player.getAudioFileDuration(),
                    (unsigned long)player.sd_min, (unsigned long)player.sd_max,
                    (unsigned long)player.shownFilePos());
    }
    else if(!strcmp(buf,"items")){ m2::M.dumpTop(); }
    else if(!strncmp(buf,"scrsm",5)){
      int v = atoi(buf + 5);
      if(buf[5] && v >= 0 && v <= 100) m2::M.smooth = (uint8_t)v;
      Serial.printf("SCRSM згладження прокрутки %u%% (0 — рівно за пальцем)\n", (unsigned)m2::M.smooth);
    }
    else if(!strcmp(buf,"scrtrace")){ m2::M.traceStart(); Serial.println("SCRTRACE запис пішов"); }
    else if(!strcmp(buf,"scrdump")){ m2::M.traceDump(); }
    else if(!strncmp(buf,"scroll ",7)){ m2::M.setScroll((int16_t)atoi(buf+7)); Serial.printf("SCROLL %d\n", atoi(buf+7)); }
    else if(!strcmp(buf,"calib")){ if(!m2::M.active()) m2::M.open(&m2::pgDev); m2::M.calibStart(); Serial.println("калібрування: торкніться чотирьох позначок"); }
    else if(!strcmp(buf,"calibreset")){ m2::M.calibReset(); Serial.println("калібрування скинуто"); }
    else if(!strncmp(buf,"tdown ",6)){ int x=0,y=0; if(sscanf(buf+6,"%d %d",&x,&y)==2){ touchscreen.injectBegin(x,y); touchscreen.loop(); Serial.println("TDOWN"); } }
    else if(!strncmp(buf,"tmove ",6)){ int x=0,y=0; if(sscanf(buf+6,"%d %d",&x,&y)==2){ touchscreen.injectMove(x,y); touchscreen.loop(); Serial.println("TMOVE"); } }
    else if(!strcmp(buf,"tup"))    { touchscreen.injectEnd(); touchscreen.loop(); Serial.println("TUP"); }
    else if(!strncmp(buf,"m2 ",3)){
      /*  нове меню одразу на сторінці: m2 <номер> (0 меню, 1 параметри, 2 екран, 3 будильник, 4 обране, 5 проповіді,
          6 про радіо, 7 живлення, 8 пояс, 9 еквалайзер, 10 обробка, 11 кімната, 12 мікрофон, 13 жести, 14 присутність,
          15 wi-fi, 16 відомі, 17 розробник, 18 заставка й звуки, 19 аудіовихід, 20 ніч з, 21 ніч до, 22 схема ЦАП,
          23 мережа, 24 оновлення, 25 станції; підсторінки 03.11: 26 колонки, 27 система, 28 нічний режим,
          29 батарея й світлодіод, 30 таймер сну, 31 гучність і баланс, 32 динамік, 33 низ, 34 хлопки/стук,
          35 що показувати, 36 обладнання, 37 MUTE, 38 логотипи, 39 сенсор, 40 налагодження, 41 заставка,
          42 звуки подій, 43 гучність кожного звуку, 44 мережі поруч)  */
      static m2::Page* const P[] = { &m2::pgPult, &m2::pgSettings, &m2::pgScreen, &m2::pgAlarm, &m2::pgFav, &m2::pgSermons,
        &m2::pgInfo, &m2::pgPower, &m2::pgTz, &m2::pgEq, &m2::pgSound, &m2::pgRoom, &m2::pgMic, &m2::pgGest, &m2::pgPres,
        &m2::pgWifi, &m2::pgSaved, &m2::pgDev, &m2::pgDevSnd, &m2::pgDac, &m2::pgNightFrom, &m2::pgNightTo, &m2::pgDacInfo,
        &m2::pgNet, &m2::pgUpdate, &m2::pgStations,
        &m2::pgSpeakers, &m2::pgSystem, &m2::pgNight, &m2::pgBatLed, &m2::pgSleep, &m2::pgSndVol, &m2::pgSndSpk, &m2::pgSndBass,
        &m2::pgGestKind, &m2::pgDevShow, &m2::pgDevHw, &m2::pgMute, &m2::pgLogos, &m2::pgTouch, &m2::pgDebug, &m2::pgSplash,
        &m2::pgSfx, &m2::pgSfxVol, &m2::pgWifiScan };
      int n = atoi(buf + 3);
      if(n >= 0 && n < (int)(sizeof(P) / sizeof(P[0]))){
        if(!m2::M.active()) m2::M.open(n == 0 ? P[0] : &m2::pgPult);
        if(n) m2::M.push(P[n]);
        Serial.printf("M2 сторінка %d\n", n);
      }
    }
    else if(!strcmp(buf,"m2close")) m2::M.closeNow();
    else if(!strncmp(buf,"ota",3)){
      /*  ota — стан; ota check / ota beta — перевірити; ota install — встановити знайдене  */
      const char* a = buf[3] == ' ' ? buf + 4 : "";
      if(!strcmp(a, "check")) ota.check(false);
      else if(!strcmp(a, "beta")) ota.check(true);
      else if(!strcmp(a, "install")) ota.install();
      Serial.printf("OTA стан=%u (%s) у радіо %s, на GitHub %s, новіша=%d, %u%% %u/%u, %u Б/с, помилка: %s\n",
        ota.state(), ota.stepName(), prVersion(), ota.latest(), ota.available(), ota.progress(),
        (unsigned)ota.done(), (unsigned)ota.total(), (unsigned)ota.speed(), ota.error());
    }
    else if(!strncmp(buf,"dspwatch ",9)){ extern volatile bool g_dspWatch; g_dspWatch = atoi(buf+9) != 0; Serial.printf("DSPWATCH %d\n", (int)g_dspWatch); }
    else if(!strncmp(buf,"specset",7)){
      float a = 0, b = 0, c = 0, e = 0;
      if(sscanf(buf + 7, "%f %f %f %f", &a, &b, &c, &e) == 4){ yoSpec.range = a; yoSpec.head = b; yoSpec.decay = c; yoSpec.avgK = e; }
      Serial.printf("SPECSET шкала=%.1f запас=%.1f спад=%.1f усередн=%.2f\n", yoSpec.range, yoSpec.head, yoSpec.decay, yoSpec.avgK);
    }
    else if(!strcmp(buf,"fade")){ Serial.printf("FADE %.3f\n", yoDsp.fadeLevel()); }
    else if(!strncmp(buf,"alarmtest ",10)){ int sec = atoi(buf + 10); if(sec >= 30 && sec <= 3600) extras.alarmTestOff((uint16_t)sec); else Serial.println("alarmtest <30..3600 с>"); }
    else if(!strcmp(buf,"aunet")){
      /*  Щосекундний звіт про потік (див. yoAuNetTick у main.cpp). Раніше тут була
          i2srate з delay(5000) — вона сама зупиняла звук: команди виконує головний цикл.  */
      extern volatile bool yoAuNet; yoAuNet = !yoAuNet;
      Serial.printf("AUNET %s; I2S %.1f Гц, потік %u Гц\n", yoAuNet ? "увімкнено" : "вимкнено",
                    (double)i2s_get_clk(I2S_NUM_0), (unsigned)player.getSampleRate());
    }
    else if(!strcmp(buf,"audiostat")){
      extern volatile uint32_t yoAuUnderN, yoAuUnderMs, yoAuMinFill;
      static uint32_t t0 = 0; uint32_t now = millis();
      Serial.printf("AUDIOSTAT за %u с: провалів %u, разом %u мс; найменше в буфері потоку %u КБ; обробка %.1f мкс/відлік; грає=%d\n",
                    (unsigned)((now - t0) / 1000), (unsigned)yoAuUnderN, (unsigned)yoAuUnderMs,
                    yoAuMinFill == 0xFFFFFFFF ? 0 : (unsigned)(yoAuMinFill / 1024), yoDsp.usPerFrame(), player.isRunning() ? 1 : 0);
      yoAuUnderN = 0; yoAuUnderMs = 0; yoAuMinFill = 0xFFFFFFFF; t0 = now;
    }
    else if(!strcmp(buf,"coredump")){
      /*  Дамп останнього падіння з флеш — без esptool і перезавантаження.
          Адреси розшифровувати addr2line з ELF тієї ж збірки (firmware/elf/).  */
      if(esp_core_dump_image_check() != ESP_OK) Serial.println("COREDUMP немає");
      else{
        esp_core_dump_summary_t* cs = (esp_core_dump_summary_t*)heap_caps_calloc(1, sizeof(esp_core_dump_summary_t), MALLOC_CAP_SPIRAM);
        if(cs && esp_core_dump_get_summary(cs) == ESP_OK){
          Serial.printf("COREDUMP задача=%s pc=0x%08lx причина=%lu адреса=0x%08lx elf=%s\n", cs->exc_task, (unsigned long)cs->exc_pc,
                        (unsigned long)cs->ex_info.exc_cause, (unsigned long)cs->ex_info.exc_vaddr, (const char*)cs->app_elf_sha256);
          Serial.print("COREDUMP стек:");
          for(uint32_t k = 0; k < cs->exc_bt_info.depth && k < 16; k++) Serial.printf(" 0x%08lx", (unsigned long)cs->exc_bt_info.bt[k]);
          Serial.printf("%s\n", cs->exc_bt_info.corrupted ? " (пошкоджено)" : "");
        }else Serial.println("COREDUMP не прочитався");
        if(cs) free(cs);
      }
    }
    else if(!strcmp(buf,"nvs")){
      /*  Чи доходять налаштування до флешу (06.11.2026: після перезапуску з оновлення радіо взяло станцію й
          гучність, яких власник уже не слухав). Лише читання: статистика NVS і звірка запису «eeprom» у флеші
          з образом у пам'яті.  */
      nvs_stats_t ns;
      if(nvs_get_stats(NULL, &ns) == ESP_OK)
        Serial.printf("NVS записів: зайнято %u, вільно %u, усього %u; просторів імен %u\n", (unsigned)ns.used_entries,
                      (unsigned)ns.free_entries, (unsigned)ns.total_entries, (unsigned)ns.namespace_count);
      Serial.printf("NVS налаштування у флеш: записано %lu, не вдалось %lu, чекає запису: %s\n", (unsigned long)config.eeOk,
                    (unsigned long)config.eeFail, config.eeDirty() ? "так" : "ні");
      nvs_handle_t h;
      if(nvs_open("eeprom", NVS_READONLY, &h) == ESP_OK){
        size_t sz = 0;
        uint8_t* b = nvs_get_blob(h, "eeprom", NULL, &sz) == ESP_OK && sz ? (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM) : nullptr;
        if(b && nvs_get_blob(h, "eeprom", b, &sz) == ESP_OK){
          int diff = 0, first = -1;
          for(size_t i = 0; i < sz && i < EEPROM.length(); i++) if(b[i] != EEPROM.read(i)){ diff++; if(first < 0) first = (int)i; }
          uint16_t st = 0; uint8_t vol = 0;
          memcpy(&st, b + config.getAddr(&config.store.lastStation), sizeof(st));
          memcpy(&vol, b + config.getAddr(&config.store.volume), sizeof(vol));
          Serial.printf("NVS «eeprom» %u Б: від пам'яті відрізняється байтів %d (перший %d); у флеші станція %u, гучність %u; "
                        "у пам'яті %u, %u\n", (unsigned)sz, diff, first, st, vol, config.store.lastStation, config.store.volume);
        }else Serial.println("NVS «eeprom»: запис не прочитався");
        if(b) free(b);
        nvs_close(h);
      }else Serial.println("NVS «eeprom»: простору імен немає");
    }
    else if(!strcmp(buf,"coredumpclr")){ esp_core_dump_image_erase(); Serial.println("COREDUMP стерто"); }
    else if(!strncmp(buf,"m2cache",7)){
      /*  m2cache 0|1 — кеш готових рядків списку станцій (порівняти m2perf)  */
      if(buf[7] == ' ') m2::m2RowCache = atoi(buf + 8) != 0;
      Serial.printf("M2CACHE %d\n", m2::m2RowCache ? 1 : 0);
    }
    else if(!strncmp(buf,"spihz",5)){
      /*  spihz <МГц> — частота шини дисплея без перезбирання (перевірка 80 МГц); без числа — показати  */
      int mhz = atoi(buf + 5);
      if(mhz >= 10 && mhz <= 80){ dsp.setSPISpeed((uint32_t)mhz * 1000000UL); spidmaHz = (uint32_t)mhz * 1000000UL; }
      /*  Справжня частота — з дільника самого SPI, а не з того, що попросили:
          частота береться діленням 80 МГц на ціле, тож не всяке число існує.  */
      const uint32_t cl = GPSPI2.clock.val;
      const uint32_t pre = ((cl >> 18) & 0x0F) + 1, n = ((cl >> 12) & 0x3F) + 1;
      const uint32_t real = (cl & 0x80000000UL) ? 80000000UL : (80000000UL / (pre * n));
      Serial.printf("SPIHZ просили %u МГц, насправді %u.%u МГц (дільник %u×%u)\n",
                    (unsigned)(spidmaHz / 1000000UL), (unsigned)(real / 1000000UL),
                    (unsigned)((real % 1000000UL) / 100000UL), (unsigned)pre, (unsigned)n);
    }
    else if(!strcmp(buf,"p2perf")){
      /*  Головний екран: кадри, смуги, пікселі й час етапів (тло, малювання, у кадр, вивід на панель).  */
      using namespace m2;
      static uint32_t t0 = 0; const uint32_t now = millis(), dt = now - t0; t0 = now;
      const uint32_t f = p2Frames ? p2Frames : 1;
      Serial.printf("P2PERF за %u мс: кадрів %u, смуг %u, пікселів %u на кадр; на кадр: тло %u мкс, малювання %u мкс, у кадр %u мкс, на панель %u мкс; найдовший кадр %u мкс\n",
                    (unsigned)dt, (unsigned)p2Frames, (unsigned)p2Strips, (unsigned)(p2Px / f), (unsigned)(p2BgUs / f), (unsigned)(p2DrawUs / f),
                    (unsigned)(p2PushUs / f), (unsigned)(p2EndUs / f), (unsigned)p2MaxUs);
      Serial.printf("P2PERF викликів render %u (%u/с), підготовка %u мкс на виклик, спектр разом %u мс, найбільший проміжок між викликами %u мс\n",
                    (unsigned)p2Calls, (unsigned)(dt ? p2Calls * 1000 / dt : 0), (unsigned)(p2Calls ? p2PreUs / p2Calls : 0), (unsigned)(p2SpecUs / 1000), (unsigned)(p2LoopGap / 1000));
      p2Frames = p2Strips = p2Px = p2BgUs = p2DrawUs = p2PushUs = p2EndUs = p2MaxUs = 0;
      p2Calls = p2PreUs = p2SpecUs = p2LoopGap = 0;
    }
    else if(!strcmp(buf,"m2perf")){
      /*  нове меню: скільки кадрів і куди йде час (з минулого виклику)  */
      static uint32_t t0 = 0;
      uint32_t now = millis(), dt = now - t0; t0 = now;
      m2::Menu& m = m2::M;
      Serial.printf("M2PERF за %u мс: кадрів %u, смуг %u, малювання %u мс, передача %u мс, tick %u мс, найдовший кадр %u мс\n",
                    (unsigned)dt, (unsigned)m.pfFrames, (unsigned)m.pfStrips, (unsigned)(m.pfDrawUs / 1000), (unsigned)(m.pfXferUs / 1000),
                    (unsigned)(m.pfTickUs / 1000), (unsigned)(m.pfMaxUs / 1000));
      m.pfFrames = m.pfStrips = m.pfDrawUs = m.pfXferUs = m.pfTickUs = m.pfMaxUs = 0;
    }
    else if(!strncmp(buf,"dmawait",7)){
      int m = atoi(buf + 7);
      if(buf[7] && m >= 1 && m <= 2) spidmaMode = (uint8_t)m;
      Serial.printf("DMAWAIT режим %u (%s), переривання %s\n", (unsigned)spidmaMode,
                    spidmaMode == 1 ? "сон за розрахунком" : "переривання",
                    spidmaIrqOn() ? "підключене" : "НЕ підключене");
    }
    else if(!strcmp(buf,"piecheck")){
      /*  Звірка векторних функцій зі звичайним кодом: усі зсуви початку
          (вирівняність) і всі довжини — саме там ховаються помилки.  */
      static uint16_t __attribute__((aligned(16))) A[200], B[200];
      uint32_t bad = 0, cases = 0;
      for(uint8_t off = 0; off < 8; off++){
        for(uint32_t n = 0; n <= 80; n++){
          /*  1. заливка  */
          for(uint32_t i = 0; i < 200; i++) A[i] = B[i] = (uint16_t)(0x1111 * (i & 7) + i);
          m2::sFill(A + off, n, 0xBEEF);
          for(uint32_t i = 0; i < n; i++) B[off + i] = 0xBEEF;
          if(memcmp(A, B, sizeof(A))) bad++;
          cases++;
          /*  2. візерунок  */
          uint16_t pat[8]; for(uint8_t k = 0; k < 8; k++) pat[k] = (uint16_t)(0xA000 + (k & 3));
          for(uint32_t i = 0; i < 200; i++) A[i] = B[i] = (uint16_t)(i * 7);
          m2::sFillPat(A + off, n, pat, (uint8_t)(off & 3));
          for(uint32_t i = 0; i < n; i++) B[off + i] = pat[((off & 3) + i) & 7];
          if(memcmp(A, B, sizeof(A))) bad++;
          cases++;
          /*  3. перестановка байтів  */
          for(uint32_t i = 0; i < 200; i++) A[i] = B[i] = (uint16_t)(i * 0x0101 + 0x1234);
          m2::sSwap16(A + off, n);
          for(uint32_t i = 0; i < n; i++){ uint16_t v = B[off + i]; B[off + i] = (uint16_t)((v >> 8) | (v << 8)); }
          if(memcmp(A, B, sizeof(A))) bad++;
          cases++;
        }
      }
      Serial.printf("PIECHECK перевірок %u, розбіжностей %u — %s\n",
                    (unsigned)cases, (unsigned)bad, bad ? "Є ПОМИЛКА" : "усе збігається");
      /*  швидкість на смузі 320×32  */
      static uint16_t* big = nullptr;
      if(!big) big = (uint16_t*)heap_caps_aligned_alloc(16, 10240 * 2, MALLOC_CAP_INTERNAL);
      if(big){
        const uint32_t N = 10240, R = 200;
        int64_t t0 = esp_timer_get_time();
        for(uint32_t k = 0; k < R; k++){ for(uint32_t i = 0; i < N; i++){ uint16_t v = big[i]; big[i] = (uint16_t)((v >> 8) | (v << 8)); } }
        int64_t t1 = esp_timer_get_time();
        for(uint32_t k = 0; k < R; k++) m2::sSwap16(big, N);
        int64_t t2 = esp_timer_get_time();
        for(uint32_t k = 0; k < R; k++){ uint16_t* p = big; for(uint32_t i = 0; i < N; i++) *p++ = 0x1234; }
        int64_t t3 = esp_timer_get_time();
        for(uint32_t k = 0; k < R; k++) m2::sFill(big, N, 0x1234);
        int64_t t4 = esp_timer_get_time();
        Serial.printf("  перестановка: було %.2f мс -> стало %.2f мс на смугу\n", (t1-t0)/1000.0/R, (t2-t1)/1000.0/R);
        Serial.printf("  заливка:      було %.2f мс -> стало %.2f мс на смугу\n", (t3-t2)/1000.0/R, (t4-t3)/1000.0/R);
      }
    }
    else if(!strcmp(buf,"pietest")){
      /*  Векторні інструкції S3 (PIE) не описані в документації Espressif —
          з'ясовуємо їхню поведінку дослідом, а не припущенням.  */
      static uint8_t __attribute__((aligned(16))) A[16], B[16], OA[16], OB[16];
      auto dump = [](const char* t, const uint8_t* p){
        Serial.printf("%-14s", t);
        for(int i = 0; i < 16; i++) Serial.printf(" %02X", p[i]);
        Serial.println();
      };
      for(int i = 0; i < 16; i++){ A[i] = i; B[i] = 0x10 + i; }
      dump("вхід A", A); dump("вхід B", B);
      __asm__ volatile("ee.vld.128.ip q0,%0,0\n ee.vld.128.ip q1,%1,0\n ee.vzip.8 q0,q1\n"
                       "ee.vst.128.ip q0,%2,0\n ee.vst.128.ip q1,%3,0\n"
                       :: "r"(A), "r"(B), "r"(OA), "r"(OB) : "memory");
      dump("zip.8  -> q0", OA); dump("zip.8  -> q1", OB);
      __asm__ volatile("ee.vld.128.ip q0,%0,0\n ee.vld.128.ip q1,%1,0\n ee.vunzip.8 q0,q1\n"
                       "ee.vst.128.ip q0,%2,0\n ee.vst.128.ip q1,%3,0\n"
                       :: "r"(A), "r"(B), "r"(OA), "r"(OB) : "memory");
      dump("unzip.8-> q0", OA); dump("unzip.8-> q1", OB);
      __asm__ volatile("ee.vld.128.ip q0,%0,0\n ee.vld.128.ip q1,%1,0\n ee.vzip.16 q0,q1\n"
                       "ee.vst.128.ip q0,%2,0\n ee.vst.128.ip q1,%3,0\n"
                       :: "r"(A), "r"(B), "r"(OA), "r"(OB) : "memory");
      dump("zip.16 -> q0", OA); dump("zip.16 -> q1", OB);
      /*  розмноження одного 16-бітного значення на весь регістр — для заливки  */
      static uint16_t __attribute__((aligned(16))) one[8] = { 0xABCD, 0, 0, 0, 0, 0, 0, 0 };
      __asm__ volatile("ee.vldbc.16 q0,%0\n ee.vst.128.ip q0,%1,0\n" :: "r"(one), "r"(OA) : "memory");
      dump("vldbc.16", OA);
      /*  множення 16-бітних доріжок: чи це молодші 16 бітів добутку?  */
      static uint16_t __attribute__((aligned(16))) M1[8] = { 100, 200, 300, 1000, 4000, 30000, 65535, 7 };
      static uint16_t __attribute__((aligned(16))) M2[8] = {   2,   3,   4,   64,   17,     3,     2, 9 };
      __asm__ volatile("ee.vld.128.ip q0,%0,0\n ee.vld.128.ip q1,%1,0\n ee.vmul.u16 q2,q0,q1\n ee.vst.128.ip q2,%2,0\n"
                       :: "r"(M1), "r"(M2), "r"(OA) : "memory");
      Serial.printf("vmul.u16      ");
      for(int i = 0; i < 8; i++) Serial.printf(" %u(чек %u)", (unsigned)((uint16_t*)OA)[i], (unsigned)(uint16_t)(M1[i] * M2[i]));
      Serial.println();
      /*  вирівнювання смуги, з якою працює DMA  */
      extern void* spidmaScratch(size_t);
      void* sc = spidmaScratch(16384);
      Serial.printf("  буфер смуги: %p, вирівняний на 16: %s\n", sc, (((uintptr_t)sc & 15) == 0) ? "так" : "НІ");
    }
    else if(!strcmp(buf,"spec")){
      extern float m2SpecDbg[32];
      Serial.print("SPEC");
      for(uint8_t k = 0; k < 32; k++) Serial.printf(" %d", (int)lroundf(m2SpecDbg[k] * 100));
      Serial.println();
    }
    else if(!strncmp(buf,"tap ",4)){
      /*  Імітація дотику: дозволяє перевірити меню без людини біля екрана. */
      char* sp = strchr(buf+4,' ');
      if(sp){ *sp=0; uint16_t x=atoi(buf+4), y=atoi(sp+1);
              Serial.printf("tap %d,%d\n", x, y);
              touchscreen.injectBegin(x,y); touchscreen.loop(); delay(80); touchscreen.loop();
              touchscreen.injectEnd(); touchscreen.loop(); delay(50); touchscreen.loop(); }
    }
    else if(!strncmp(buf,"list",4)) display.putRequest(NEWMODE, STATIONS);
    else if(!strncmp(buf,"drag ",5)){
      /*  drag <x> <y0> <y1> <мс> — веде «палець» по прямій і відпускає.
          Йде тим самим шляхом, що й справжній дотик.  */
      int x=0,y0=0,y1=0,ms=400;
      if(sscanf(buf+5, "%d %d %d %d", &x,&y0,&y1,&ms) >= 3){
        const int steps = ms/15 < 4 ? 4 : ms/15;
        Serial.printf("drag %d: %d -> %d за %d мс, %d кроків\n", x, y0, y1, ms, steps);
        touchscreen.injectBegin(x, y0);
        touchscreen.loop();
        for(int i=1;i<=steps;i++){
          delay(ms/steps);
          touchscreen.injectMove(x, y0 + (y1-y0)*i/steps);
          touchscreen.loop();
        }
        touchscreen.injectEnd();
        for(int i=0;i<120;i++){ delay(15); touchscreen.loop(); }   /* даємо накату доїхати */
        Serial.printf("після жесту: станція %d\n", display.currentPlItem);
      }
    }
    else if(!strncmp(buf,"sdown ",6)){ int x=0,y=0; if(sscanf(buf+6, "%d %d", &x,&y)==2){ touchscreen.injectBegin(x,y); Serial.printf("палець %d,%d тримає\n", x, y); } }
    else if(!strncmp(buf,"smove ",6)){ int x=0,y=0; if(sscanf(buf+6, "%d %d", &x,&y)==2) touchscreen.injectMove(x,y); }
    else if(!strcmp(buf,"sup")){ touchscreen.injectEnd(); Serial.println("палець відпущено"); }
    else if(!strncmp(buf,"stap ",5)){
      int x=0,y=0;
      if(sscanf(buf+5, "%d %d", &x,&y)==2){
        Serial.printf("дотик %d,%d\n", x, y);
        touchscreen.injectBegin(x,y); touchscreen.loop();
        delay(80); touchscreen.loop();
        touchscreen.injectEnd(); touchscreen.loop();
        delay(50); touchscreen.loop();
      }
    }
    /*  ---- доповнення: сон, будильник, ніч, батарея, світлодіод ---- */
    else if(!strcmp(buf,"ext")){
      ExtStore& s = extras.s;
      Serial.printf("сон: %u хв, лишилось %u с; будильник %s %02u:%02u %s, через %d хв, дзвонить=%d\n",
        extras.sleepMinutes(), (unsigned)extras.sleepLeftSec(), s.alarmOn?"увімк":"вимк",
        s.alarmH, s.alarmM, s.alarmDays?"будні":"щодня", (int)extras.alarmInMin(), extras.alarmRinging()?1:0);
      Serial.printf("ніч: %s %02u:%02u..%02u:%02u рівень %u, зараз ніч=%d темно=%d; підсвітка ціль %u\n",
        s.nightOn?"увімк":"вимк", s.nightFrom/2, (s.nightFrom%2)*30, s.nightTo/2, (s.nightTo%2)*30,
        s.nightLevel, extras.nightActive()?1:0, extras.dark()?1:0, extras.pwmTarget());
#if BRIGHTNESS_PIN!=255
      Serial.printf("підсвітка зараз: шпаруватість %u з 255, ШІМ %u Гц\n", (unsigned)ledcRead(BRIGHTNESS_PIN), (unsigned)ledcReadFreq(BRIGHTNESS_PIN));
#endif
      Serial.printf("батарея %u мВ %d%% usb=%d; світлодіод %u; гучність %u override %d\n",
        extras.batMv(), extras.batPct(), extras.onUsb()?1:0, s.ledMode, config.store.volume, (int)player.volOverride);
    }
    else if(!strncmp(buf,"batsave ",8)){ extras.s.batSave = (uint8_t)atoi(buf+8) % 5; extras.changed(); Serial.printf("економія: %u\n", extras.s.batSave); }
    else if(!strncmp(buf,"batsim ",7)){
      int pc=-1,ch=-1; sscanf(buf+7, "%d %d", &pc, &ch);
      extras.batSim(pc, ch); Serial.printf("батарея: підставлено %d%%, заряджання %d\n", pc, ch);
    }
    else if(!strcmp(buf,"bat")){
      for(int i=0;i<8;i++){ Serial.printf("GPIO9: %u мВ (x2)\n", extras.batRaw()); delay(100); }
    }
    else if(!strncmp(buf,"sleeptest ",10)) { extras.sleepTestSec(atoi(buf+10)); Serial.println("таймер сну запущено"); }
    else if(!strncmp(buf,"sleep ",6))      { extras.setSleep(atoi(buf+6)); Serial.println("таймер сну"); }
    else if(!strcmp(buf,"alarmnow"))       { extras.alarmNow(); Serial.println("будильник запущено"); }
    else if(!strncmp(buf,"shold ",6)){
      /*  shold <x> <y> <мс> — утримати палець  */
      int x=0,y=0,ms=0;
      if(sscanf(buf+6, "%d %d %d", &x,&y,&ms)==3){
        touchscreen.injectBegin(x,y); touchscreen.loop();
        uint32_t t0 = millis();
        while(millis() - t0 < (uint32_t)ms){ delay(15); touchscreen.loop(); }
        touchscreen.injectEnd(); touchscreen.loop();
        delay(50); touchscreen.loop();
        Serial.printf("утримано %d,%d %d мс\n", x, y, ms);
      }
    }
    else if(!strcmp(buf,"rec"))            { if(recorder.active()) recorder.stop(); else if(!recorder.start()) Serial.printf("запис: %s\n", recorder.lastError()); }
    else if(!strcmp(buf,"recst"))          { Serial.printf("запис=%d %s %u байт %u с\n", recorder.active(), recorder.fileName(), (unsigned)recorder.bytes(), (unsigned)recorder.seconds()); }
    else if(!strcmp(buf,"serm"))           { sermons.fetch(); Serial.println("завантаження проповідей"); }
    else if(!strncmp(buf,"sermplay ",9))   { const int i = atoi(buf + 9); Serial.printf("SERMPLAY %d: %s\n", i, sermons.play(i) ? "грає" : "недоступна"); }
    else if(!strcmp(buf,"sermls")){
      for(uint16_t i=0;i<sermons.count();i++){ const Sermon* s=sermons.at(i); Serial.printf("%2u %s | %s | %s | %u\n", i, s->date, s->title, s->preacher, s->dur); }
      Serial.printf("усього %u, помилка '%s', вантажиться=%d\n", sermons.count(), sermons.error(), sermons.loading());
    }
    else if(!strncmp(buf,"night ",6))      { extras.forceNight(atoi(buf+6)); Serial.println("ніч примусово"); }
    else if(!strncmp(buf,"led ",4)){
      int r=0,g=0,b=0;
      if(sscanf(buf+4, "%d %d %d", &r,&g,&b)==3){ extras.ledTest(r,g,b,3000); Serial.println("світлодіод 3 с"); }
    }
    else if(!strcmp(buf,"plstate")) touchscreen.dbgState();
    else if(!strcmp(buf,"reboot")) ESP.restart();
    else if(!strncmp(buf,"eetest",6)){
      /*  Скільки стоїть радіо, поки налаштування пишуться у флеш (EEPROM поверх NVS): міняємо
          останній, невживаний байт області й зберігаємо N разів, потім повертаємо як було.  */
      static_assert(EEPROM_START + sizeof(config_t) < EEPROM_SIZE - 1, "eetest: байт поза налаштуваннями");
      int n = buf[6] == ' ' ? atoi(buf + 7) : 5; if(n < 1) n = 1; if(n > 20) n = 20;
      const int a = EEPROM_SIZE - 1;
      const uint8_t was = EEPROM.read(a);
      uint32_t mx = 0, sum = 0;
      for(int i = 0; i <= n; i++){
        EEPROM.write(a, i == n ? was : (uint8_t)(was ^ (1 + (i & 1))));
        const int64_t t0 = esp_timer_get_time();
        EEPROM.commit();
        const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        if(us > mx) mx = us; sum += us;
        Serial.printf("EETEST запис %d: %u мкс\n", i + 1, (unsigned)us); Serial.flush();
        delay(150);
      }
      Serial.printf("EETEST %d записів: найдовший %u мкс, у середньому %u мкс; байт повернуто (%u)\n", n + 1, (unsigned)mx, (unsigned)(sum / (n + 1)), (unsigned)EEPROM.read(a));
    }
    else if(!strncmp(buf,"cmd ",4)){
      /*  будь-яка команда yoRadio, як зі сторінки чи http://радіо/?play=37: «cmd play=37»  */
      char* eq = strchr(buf + 4, '=');
      if(!eq) Serial.println("cmd назва=значення");
      else{ *eq = 0; extern CommandHandler cmd; Serial.printf("CMD %s=%s: %s\n", buf + 4, eq + 1, cmd.exec(buf + 4, eq + 1) ? "так" : "ні"); }
    }
    else Serial.printf("DBG: невідома команда '%s'\n", buf);
  }
}
#endif
