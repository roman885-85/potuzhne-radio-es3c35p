#include "options.h"
#if SDC_CS!=255
#include <Arduino.h>
#include <SPI.h>
#if SD_SDMMC
  #include <SD_MMC.h>
#else
  #include <SD.h>
  #include "sd_diskio.h"
#endif
#include "vfs_api.h"
#if SD_SDMMC
  #include "esp_vfs_fat.h"
  #include "esp_vfs.h"
#endif
//#define USE_SD
#include "config.h"
#include "sdmanager.h"
#include "display.h"
#include "player.h"

#if defined(SD_SPIPINS) || SD_HSPI
SPIClass  SDSPI(HSPI);
#define SDREALSPI SDSPI
#else
  #define SDREALSPI SPI
#endif

#ifndef SDSPISPEED
  #define SDSPISPEED 20000000
#endif

SDManager sdman(FSImplPtr(new VFSImpl()));

#if SD_SDMMC
/*  Карта на ES3C28P сидить не на SPI, а на чотирибітній шині SDIO.
 *
 *  Важливо: невдалий begin() лишає всередині SDMMCFS ненульовий _card, а
 *  setPins() у такому стані відмовляється працювати («must be called before
 *  begin»). Тому перед кожною спробою робимо end() — інакше одна невдача
 *  назавжди закриває доступ до картки до перезавантаження.                  */
bool SDManager::start(){
  if(ready) return true;
  /*  ESP-IDF у ядрі Arduino 3.3.3 (release/v5.5 f1a1df9b, перевірено розбором машинного коду
      libvfs.a): верхня межа таблиці VFS лише росте — esp_vfs_unregister_with_id її не зменшує, —
      а реєстрація відмовляє (0x101, «mount_to_vfs failed»), щойно межа дійшла до 8, навіть коли
      нижче є вільне місце. На старті в таблиці 7 записів, картка бере восьмий, і після першого
      ж відмонтування вже не монтувалась до перезапуску (власник 26.10.2026: «вийняв і вставив
      — картка не працює»; відтворено командою sdcycle). Звільняємо /dev/null (його ніхто не
      відкриває): тоді картка щоразу сідає на його місце, нижче межі, і межа не росте.  */
  static bool vfsFreed = false;
  if(!vfsFreed){
    vfsFreed = true;
    const esp_err_t e = esp_vfs_unregister("/dev/null");
    if(e != ESP_OK) Serial.printf("##SD#\t/dev/null не звільнено: 0x%x\n", (unsigned)e);
  }

  end();                                   /* скидаємо _card від попередніх спроб */
  if(!setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_D0, SDMMC_D1, SDMMC_D2, SDMMC_D3)){
    Serial.println("##[ERROR]#\tSD_MMC: не вдалося призначити виводи");
    return false;
  }
  ready = begin("/sdcard", false, false, BOARD_MAX_SDMMC_FREQ, 5);

  if(!ready){                              /* один біт і менша частота — терпиміше */
    end();
    setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_D0, SDMMC_D1, SDMMC_D2, SDMMC_D3);
    ready = begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 5);
    if(ready) Serial.println("##[BOOT]#\tSD: однобітний режим");
  }
  if(!ready){
    end();
    Serial.println("##[ERROR]#\tSD: картку не знайдено");
  }
  return ready;
}

#else
bool SDManager::start(){
  ready = begin(SDC_CS, SDREALSPI, SDSPISPEED);
  vTaskDelay(10);
  if(!ready) ready = begin(SDC_CS, SDREALSPI, SDSPISPEED);
  vTaskDelay(20);
  if(!ready) ready = begin(SDC_CS, SDREALSPI, SDSPISPEED);
  vTaskDelay(50);
  if(!ready) ready = begin(SDC_CS, SDREALSPI, SDSPISPEED);
  return ready;
}
#endif

/*  Відмонтовуємо самі й дивимось, чи вдалось: ядро (SDMMCFS::end) відповіді не перевіряє, і
    коли відмонтування не проходило, запис файлової системи лишався зайнятим — кожне наступне
    монтування падало «mount_to_vfs failed (0x101)», і картка після «вийняли й вставили»
    не працювала до перезапуску (звіт власника 26.10.2026; відтворено командою sdcycle).  */
void SDManager::stop(){
#if SD_SDMMC
  if(_card){
    const esp_err_t e = esp_vfs_fat_sdcard_unmount("/sdcard", _card);
    if(e != ESP_OK) Serial.printf("##SD#\tвідмонтувати не вдалось: 0x%x\n", (unsigned)e);
    else _card = nullptr;                  /* уже звільнено — end() лише прибере виводи */
    if(!_card){ _impl->mountpoint(NULL); }
  }
#endif
  end();
  ready = false;
}
#if !SD_SDMMC
#include "diskio_impl.h"
#endif
bool SDManager::cardPresent() {

  if(!ready) return false;
  if(sectorSize()<1) {
    return false;
  }
  uint8_t buff[sectorSize()] = { 0 };
  bool bread = readRAW(buff, 1);
  if(sectorSize()>0 && !bread) return false;
  return bread;
}

bool SDManager::_checkNoMedia(const char* path){
  if (path[strlen(path) - 1] == '/')
    snprintf(config.tmpBuf, sizeof(config.tmpBuf), "%s%s", path, ".nomedia");
  else
    snprintf(config.tmpBuf, sizeof(config.tmpBuf), "%s/%s", path, ".nomedia");
  bool nm = exists(config.tmpBuf);
  return nm;
}

bool SDManager::_endsWith (const char* base, const char* str) {
  int slen = strlen(str) - 1;
  const char *p = base + strlen(base) - 1;
  while(p > base && isspace(*p)) p--;
  p -= slen;
  if (p < base) return false;
  return (strncmp(p, str, slen) == 0);
}

/*  Розширення — без різниці великими чи малими. Раніше назву файла приводили до малих літер
    просто в шляху (strlwr), і в списку картки стояло «00000001.wav» замість «00000001.WAV».  */
static bool extIs(const char* fn, const char* ext){
  const size_t a = strlen(fn), b = strlen(ext);
  return a > b && !strcasecmp(fn + a - b, ext);
}

/*  Записи пульта (Behringer X-LIVE): «…/<сесія>/00000001.WAV», де сесія — 8 шістнадцяткових
    цифр, час її початку (секунди від 1970 р. за годинником пульта). Усі файли звуться однаково,
    тож у списку замість семи «00000001.WAV» — дата й тека: «18.04.2019 09:51 · X_LIVE_троица».  */
static bool xliveName(const char* path, const char* fn, char* out, size_t n){
  if(strlen(fn) != 12 || strcasecmp(fn + 8, ".wav")) return false;
  for(uint8_t i = 0; i < 8; i++) if(!isdigit((unsigned char)fn[i])) return false;
  const char* slash = fn - 1;                          /* «/» перед назвою */
  if(slash - path < 9 || slash[-9] != '/') return false;
  const char* dir = slash - 8;
  uint32_t t = 0;
  for(uint8_t i = 0; i < 8; i++){
    const char c = dir[i];
    if(!isxdigit((unsigned char)c)) return false;
    t = t * 16 + (uint32_t)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
  }
  if(t < 946684800UL) return false;                    /* раніше 2000 року — не час, а щось інше */
  const char* gpEnd = dir - 1;                          /* «/» перед текою сесії */
  const char* gp = gpEnd;
  while(gp > path && gp[-1] != '/') gp--;
  time_t tt = (time_t)t; struct tm tv; gmtime_r(&tt, &tv);
  const int part = atoi(fn);
  char tail[12] = { 0 };
  if(part > 1) snprintf(tail, sizeof(tail), " · %d", part);
  snprintf(out, n, "%02d.%02d.%04d %02d:%02d%s%.*s%s", tv.tm_mday, tv.tm_mon + 1, tv.tm_year + 1900, tv.tm_hour, tv.tm_min,
           gpEnd > gp ? " · " : "", (int)(gpEnd - gp), gp, tail);
  return true;
}

void SDManager::listSD(File &plSDfile, File &plSDindex, const char* dirname, uint8_t levels) {
    File root = sdman.open(dirname);
    if (!root) {
        Serial.println("##[ERROR]#\tFailed to open directory");
        return;
    }
    if (!root.isDirectory()) {
        Serial.println("##[ERROR]#\tNot a directory");
        return;
    }

    uint32_t pos = 0;
    char* filePath;
    while (true) {
        vTaskDelay(2);
        player.loop();
        bool isDir;
        String fileName = root.getNextFileName(&isDir);
        if (fileName.isEmpty()) break;
        filePath = (char*)malloc(fileName.length() + 1);
        if (filePath == NULL) {
            Serial.println("Memory allocation failed");
            break;
        }
        strcpy(filePath, fileName.c_str());
        const char* fn = strrchr(filePath, '/') + 1;
        /*  Службові файли й теки, що лишають на флешці macOS і Windows.
            «._назва.m4a» — це метадані Finder, а не музика, але розширення в
            них те саме, і плейлист брав їх за треки: перемикання на такий
            «трек» давало тишу. Приховані теки (.Trashes, .Spotlight-V100)
            теж пропускаємо.  */
        if (fn[0] == '.' || strcmp(fn, "System Volume Information") == 0 || strcmp(fn, "$RECYCLE.BIN") == 0) {
          free(filePath);
          continue;
        }
        if (isDir) {
            if (levels && !_checkNoMedia(filePath)) {
                listSD(plSDfile, plSDindex, filePath, levels - 1);
            }
        } else {
            if (extIs(fn, ".mp3") || extIs(fn, ".m4a") || extIs(fn, ".aac") ||
                extIs(fn, ".wav") || extIs(fn, ".flac")) {
                pos = plSDfile.position();
                char xl[96];
                plSDfile.printf("%s\t%s\t0\n", xliveName(filePath, fn, xl, sizeof(xl)) ? xl : fn, filePath);
                plSDindex.write((uint8_t*)&pos, 4);
                Serial.print(".");
                if(display.mode()==SDCHANGE) display.putRequest(SDFILEINDEX, _sdFCount+1);
                _sdFCount++;
                if (_sdFCount % 64 == 0) Serial.println();
            }
        }
        free(filePath);
    }
    root.close();
}

/*  Картку могли переписати на комп'ютері, а індекс лишався старий — радіо показувало не ті
    файли (власник 26.10.2026: «бачить не всі файли»). Відбиток картки — повний розмір і
    зайняте місце; не збігся з тим, що записано при останньому індексі, — індексуємо заново.
    Записи ефіру теж міняють зайняте місце, тож нові записи з'являються в списку самі.  */
void SDManager::_sig(char* b, size_t n){
  snprintf(b, n, "%llu %llu", (unsigned long long)totalBytes(), (unsigned long long)usedBytes());
}
bool SDManager::sigSame(){
  if(!exists(SIG_SD_PATH)){                       /* заготовка: файл займе своє місце ще до заміру */
    File w = open(SIG_SD_PATH, "w", true); if(w){ w.print("0 0"); w.close(); }
    return false;
  }
  char have[48] = { 0 }, now[48];
  File f = open(SIG_SD_PATH, "r");
  if(f){ f.readBytes(have, sizeof(have) - 1); f.close(); }
  _sig(now, sizeof(now));
  return !strcmp(have, now);
}
void SDManager::_sigSave(){
  char now[48]; _sig(now, sizeof(now));
  File w = open(SIG_SD_PATH, "w", true);
  if(w){ w.print(now); w.close(); }
}

void SDManager::indexSDPlaylist() {
  _sdFCount = 0;
  if(exists(PLAYLIST_SD_PATH)) remove(PLAYLIST_SD_PATH);
  if(exists(INDEX_SD_PATH)) remove(INDEX_SD_PATH);
  File playlist = open(PLAYLIST_SD_PATH, "w", true);
  if (!playlist) {
    return;
  }
  File index = open(INDEX_SD_PATH, "w", true);
  listSD(playlist, index, "/", SD_MAX_LEVELS);
  index.flush();
  index.close();
  playlist.flush();
  playlist.close();
  _sigSave();                                     /* відбиток — уже з новим індексом на картці */
  Serial.println();
  delay(50);
}
#endif


