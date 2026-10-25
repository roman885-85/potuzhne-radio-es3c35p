#include "../core/options.h"
#ifdef YO_DEBUG
#include "yoHwTest.h"
#include <WiFi.h>
#include <SPIFFS.h>
#include <LittleFS.h>
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "../core/config.h"
#include "../core/player.h"
#include "../core/network.h"
#include "../core/sdmanager.h"
#include "../core/display.h"
#include "../displays/dspcore.h"
#include "../ES8311/yoES8311.h"
#include "yoExtras.h"
#include "yoMic.h"
#include "yoOta.h"
#include "yoVersion.h"
#include "yoSfx.h"
#if TS_MODEL==TS_MODEL_ST77922
  #include "../ST77922_Touch/yoST77922Touch.h"
  extern YoST77922Touch ts;
#endif

static uint8_t s_ok = 0, s_bad = 0, s_warn = 0;
static void line(const char* node, int8_t st, const char* fmt, ...){
  char b[200];
  va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
  Serial.printf("HWTEST %-11s %s  %s\n", node, st > 0 ? "РОБОТА" : st == 0 ? "УВАГА " : "ЗБІЙ  ", b);
  if(st > 0) s_ok++; else if(st == 0) s_warn++; else s_bad++;
}

void yoHwTest(const char* arg){
  (void)arg;
  s_ok = s_bad = s_warn = 0;
  const uint32_t t0 = millis();
  Serial.printf("HWTEST ПОЧАТОК: плата %s, прошивка %s від %s\n", PR_BOARD, prVersion(), prBuild());

  /*  процесор і пам'ять  */
  esp_chip_info_t ci; esp_chip_info(&ci);
  line("чип", 1, "ESP32-S3 rev %d.%d, %u МГц, ядер %d", ci.revision / 100, ci.revision % 100, (unsigned)ESP.getCpuFreqMHz(), ci.cores);
  line("флеш", ESP.getFlashChipSize() >= 16u * 1024 * 1024 ? 1 : -1, "%u МБ", (unsigned)(ESP.getFlashChipSize() >> 20));
  {
    const size_t ps = ESP.getPsramSize(), pf = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    line("PSRAM", ps >= 8u * 1024 * 1024 - 65536 ? 1 : -1, "%u КБ, вільно %u КБ", (unsigned)(ps / 1024), (unsigned)(pf / 1024));
    const size_t inf = heap_caps_get_free_size(MALLOC_CAP_INTERNAL), inm = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 inl = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    line("ОЗП", inm > 30000 ? 1 : inm > 20000 ? 0 : -1, "внутрішня вільна %u КБ, мінімум %u КБ, найбільший шмат %u КБ", (unsigned)(inf / 1024), (unsigned)(inm / 1024), (unsigned)(inl / 1024));
  }

  /*  екран  */
#if DSP_MODEL==DSP_ST77922
  {
    const bool rc = lcdRotCheck();                 /* друкує свій рядок ROTCHECK */
    line("екран", rc ? 1 : -1, "ST77922 QSPI %u МГц, %ux%u, поворот %u, поворот смуг PIE %s", (unsigned)(st77922Hz() / 1000000), DSP_W, DSP_H, st77922Rot(), rc ? "збігається зі звичайним" : "РОЗБІЖНІСТЬ");
    lcdStats(false);
  }
#else
  line("екран", 1, "ILI9341 %ux%u", DSP_W, DSP_H);
#endif

  /*  сенсор  */
#if TS_MODEL==TS_MODEL_ST77922
  line("сенсор", ts.present() ? 1 : -1, "ST77922 0x55: версія %u (%u.%u.%u.%u), опитів %u, звітів %u, дотиків %u, помилок I2C %u",
       ts.fwVer, ts.fwRev[0], ts.fwRev[1], ts.fwRev[2], ts.fwRev[3], (unsigned)ts.reads, (unsigned)ts.reports, (unsigned)ts.touchesN, (unsigned)ts.errors);
#endif

  /*  кодек і звук  */
#if I2S_ES8311
  {
    const int id = es8311_id();
    line("кодек", id == 0x83 ? 1 : -1, "ES8311 id=0x%02X, налаштований на %u Гц, %s", id < 0 ? 0 : id, (unsigned)es8311_rate(), es8311_ready() ? "готовий" : "НЕ ініціалізований");
  }
#endif
  line("підсилювач", 1, "вивід %d = %s", MUTE_PIN, digitalRead(MUTE_PIN) == MUTE_VAL ? "вимкнено (тиша)" : "увімкнено");
  line("плеєр", 1, "%s, режим %s, станція %u, гучність %u, %u кбіт/с %s", player.status() == PLAYING ? "грає" : "стоїть",
       config.getMode() == PM_SDCARD ? "картка" : "радіо", (unsigned)config.lastStation(), (unsigned)config.store.volume,
       (unsigned)config.station.bitrate, player.status() == PLAYING ? player.getCodecname() : "");
  line("мікрофон", 1, "%s, рівень %d дБ", mic.listening() ? "слухає" : "не слухає", (int)mic.levelDb());
  line("звуки", YoSfx::mounted() ? 1 : 0, "розділ ресурсів %s", YoSfx::mounted() ? "змонтовано" : "НЕ змонтовано");

  /*  картка пам'яті  */
  if(sdman.ready) line("картка", 1, "%llu МБ, зайнято %llu МБ", sdman.cardSize() / (1024 * 1024), sdman.usedBytes() / (1024 * 1024));
  else line("картка", 0, "не змонтована (картки немає?)");

  /*  батарея, світлодіод  */
  line("батарея", extras.batMv() >= 2800 ? 1 : 0, "%u мВ, %d%%, %s", (unsigned)extras.batMv(), (int)extras.batPct(), extras.onPower() ? "живлення від USB" : "від батареї");
  line("світлодіод", 1, "WS2812 на виводі %d, режим %u", EXT_LED_PIN, (unsigned)extras.s.ledMode);

  /*  мережа  */
  if(WiFi.status() == WL_CONNECTED){
    line("Wi-Fi", WiFi.RSSI() > -80 ? 1 : 0, "«%s», %d дБм, канал %d, %s", WiFi.SSID().c_str(), WiFi.RSSI(), WiFi.channel(), WiFi.localIP().toString().c_str());
    IPAddress ip; uint32_t d0 = millis();
    const bool dns = WiFi.hostByName("api.github.com", ip) == 1;
    line("інтернет", dns ? 1 : -1, "api.github.com → %s за %u мс", dns ? ip.toString().c_str() : "немає відповіді DNS", (unsigned)(millis() - d0));
  }else line("Wi-Fi", 0, "не підключено (стан %d)", (int)WiFi.status());
  {
    struct tm t; const bool ok = getLocalTime(&t, 10);
    line("годинник", ok ? 1 : 0, ok ? "%02d:%02d:%02d %02d.%02d.%04d" : "час ще не звірено", t.tm_hour, t.tm_min, t.tm_sec, t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
  }

  /*  оновлення й файли  */
  line("оновлення", 1, "файл випуску «PotuzhneRadio-%s-update.bin», остання на GitHub: %s", PR_BOARD, ota.latest()[0] ? ota.latest() : "ще не перевіряли");
  line("SPIFFS", SPIFFS.totalBytes() ? 1 : -1, "%u з %u КБ", (unsigned)(SPIFFS.usedBytes() / 1024), (unsigned)(SPIFFS.totalBytes() / 1024));

  Serial.printf("HWTEST ПІДСУМОК: працює %u, увага %u, збоїв %u — за %u мс\n", s_ok, s_warn, s_bad, (unsigned)(millis() - t0));
}
#else
void yoHwTest(const char*){}
#endif
