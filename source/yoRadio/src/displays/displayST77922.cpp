/*  ---------------------------------------------------------------------------
 *  ES3C35P: екран ST77922 (320×480, QSPI) у ландшафті 480×320.
 *
 *  Шина. QSPI на SPI2 через рідні виводи IO_MUX (12 SCK, 11/13/14/9 D0..D3,
 *  10 CS), 80 МГц — як у прикладах LCDwiki. Регістри пишуться кодом 0x02
 *  (команда, адреса й дані — по одній лінії), пікселі — кодом 0x32 (дані по
 *  чотирьох лініях): адреса 0x002C00 — запис з початку вікна, 0x003C00 —
 *  продовження. Читання регістрів (0x0B) — з одним тактом очікування; кадр у
 *  панелі зберігається стиснутим блоками по 4 пікселі, тому
 *    1) стовпці вікна (CASET) мусять починатися й кінчатися на межі 4 пікселів,
 *    2) відеопам'ять назад читається лише стиснутою — знімки беремо з «тіні».
 *
 *  Поворот. У режимі QSPI панель не вміє міняти осі (MADCTL MV; так і в драйвері
 *  esp_lcd_st77922 Espressif), тож смуга, намальована в ландшафті, перед
 *  відправкою повертається на 90° у власний буфер. Поворот — векторними
 *  інструкціями ESP32-S3 (PIE, ті самі, що Espressif називає «ШІ-прискоренням»):
 *  блок 8×8 — 8 завантажень, 12 ee.vzip.16, 8 команд перестановки байтів і
 *  8 записів. Звичайний код лишено для звірки (команда rotcheck) і для смуг,
 *  що не кратні 8.
 *
 *  Відповідність осей (перевірено на платі 25.10.2026: з USB праворуч червоний
 *  квадрат «верх-ліво» стоїть угорі ліворуч, дотики в кути збіглися):
 *    поворот 1:  панель px = 319 − y, py = x
 *    поворот 3:  панель px = y,       py = 479 − x   (перевернуто, «Перевернути екран»)
 *
 *  Кадр цілком і такт панелі (25.10.2026). Власник: «при прокрутці списків усе
 *  рветься й смикається». Смуги йшли в панель одна за одною, щойно намальовані,
 *  а кадр тривав ~21 мс — довше за прохід розгортки (~16 мс): панель показувала
 *  частину нового кадру й частину старого. До того ж розгортка ST77922 іде
 *  рядками ПАНЕЛІ, тобто стовпцями нашого ландшафту, — поперек смуг.
 *  Тепер смуги лише кладуться в кадр у PSRAM (він же «тінь» для знімків), а в
 *  кінці кадру (lcdEnd) змінена частина йде в панель одним вікном, рядками панелі
 *  в порядку розгортки, починаючи з гасіння (сигнал TE, GPIO 42). Запис усього
 *  екрана (~8–10 мс) швидший за розгортку (~13 мс), тож вона його не наздоганяє
 *  і на екрані завжди цілий кадр — так робить і приклад виробника (esp_bsp.c).
 *  ------------------------------------------------------------------------- */
#include "../core/options.h"
#if DSP_MODEL==DSP_ST77922
#include "dspcore.h"
#include "tools/lcdout.h"
#include "tools/spidma.h"
#include "../core/config.h"
#include "../m2/m2simd.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "hal/gpio_ll.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/semphr.h"

static const int16_t PW = 320, PH = 480;          /* панель у власній (портретній) системі */

/* ---------- послідовність ініціалізації виробника ---------- */
struct InitCmd { uint8_t cmd; const uint8_t* data; uint8_t len; uint16_t delay_ms; };
#define D(s) ((const uint8_t*)(s))
/*  Із прикладу LCDwiki Example_08 (esp_bsp.c) — тим, що стоїть на заводській
    прошивці; Example_01 відрізняється лише 0x71 і 0xBE.  */
static const InitCmd INIT[] = {
  {0xF1, D("\x00"), 1, 0},
  {0x60, D("\x00\x00\x00"), 3, 0},
  {0x65, D("\x80"), 1, 0},
  {0x79, D("\x06"), 1, 0},
  {0x7B, D("\x00\x08\x08"), 3, 0},
  {0x80, D("\x55\x62\x2F\x17\xF0\x52\x70\xD2\x52\x62\xEA"), 11, 0},
  {0x81, D("\x26\x52\x72\x27"), 4, 0},
  {0x84, D("\x92\x25"), 2, 0},
  {0x87, D("\x10\x10\x58\x00\x02\x3A"), 6, 0},
  {0x88, D("\x00\x00\x2C\x10\x04\x00\x00\x00\x01\x01\x01\x01\x01\x00\x06"), 15, 0},
  {0x89, D("\x00\x00\x00"), 3, 0},
  {0x8A, D("\x13\x00\x2C\x00\x00\x2C\x10\x10\x00\x3E\x19"), 11, 0},
  {0x8B, D("\x15\xB1\xB1\x44\x96\x2C\x10\x97\x8E"), 9, 0},
  {0x8C, D("\x1D\xB1\xB1\x44\x96\x2C\x10\x50\x0F\x01\xC5\x12\x09"), 13, 0},
  {0x8D, D("\x0C"), 1, 0},
  {0x8E, D("\x33\x01\x0C\x13\x01\x01"), 6, 0},
  {0xB3, D("\x00\x30"), 2, 0},
  {0xF1, D("\x00"), 1, 0},
  {0x71, D("\xC0"), 1, 0},
  {0x66, D("\x02\x3F"), 2, 0},
  {0xBE, D("\x24\x00\x9D"), 3, 0},
  {0x70, D("\x01\xA0\x11\x40\xE0\x00\x11\x69\x11\x00\x00\x1A"), 12, 0},
  {0x90, D("\x04\x04\x55\x74\x00\x40\x43\x27\x27"), 9, 0},
  {0x91, D("\x04\x04\x55\x75\x00\x40\x42\x27\x27"), 9, 0},
  {0x92, D("\x04\x44\x55\xC0\x06\x00\x07\x05\x90\x27"), 10, 0},
  {0x93, D("\x04\x43\x11\x00\x00\x00\x00\x05\x90\x27"), 10, 0},
  {0x94, D("\x00\x00\x00\x00\x00\x00"), 6, 0},
  {0x95, D("\x96\x16\x00\x00\xFF"), 5, 0},
  {0x96, D("\x44\x53\x03\x12\x23\x24\x06\x05\x94\x27\x00\x44"), 12, 0},
  {0x97, D("\x44\x53\x47\x56\x20\x20\x02\x01\x94\x27\x00\x44"), 12, 0},
  {0xBA, D("\x55\x94\x2D\x94\x27"), 5, 0},
  {0x9A, D("\x40\x00\x06\x00\x00\x00\x00"), 7, 0},
  {0x9B, D("\x00\x00\x06\x00\x00\x00\x00"), 7, 0},
  {0x9C, D("\x5C\x12\x00\x00\x10\x12\x00\x00\x10\x02\x00\x00\x00"), 13, 0},
  {0x9D, D("\x8A\x51\x00\x00\x00\x80\x1E\x01"), 8, 0},
  {0x9E, D("\x51\x00\x00\x00\x80\x1E\x01"), 7, 0},
  {0xB4, D("\x1D\x1C\x1E\x0B\x14\x02\x13\x09\x1E\x00\x1E\x10"), 12, 0},
  {0xB5, D("\x1D\x1C\x1E\x0A\x15\x03\x11\x08\x1E\x01\x1E\x12"), 12, 0},
  {0xB6, D("\x77\x77\x00\x0A\xFF\x0A\xFF"), 7, 0},
  {0x86, D("\xCD\x04\xB1\x02\x58\x12\x58\x0C\x13\x01\xA5\x00\xA5\xA5"), 14, 0},
  {0xB7, D("\x07\x0A\x0E\x06\x05\x03\x2B\x03\x03\x42\x07\x10\x10\x2E\x3F\x0D"), 16, 0},
  {0xB8, D("\x07\x0A\x0D\x05\x05\x02\x2B\x02\x03\x42\x06\x10\x0F\x2E\x3F\x0D"), 16, 0},
  {0xB9, D("\x23\x23"), 2, 0},
  {0xBF, D("\x10\x14\x14\x0B\x0B\x0B"), 6, 0},
  {0xF2, D("\x00"), 1, 0},
  {0x73, D("\x04\xDA\x12\x54\x47"), 5, 0},
  {0x77, D("\x6B\x5B\xFD\xC3\xC5"), 5, 0},
  {0x7A, D("\x15\x27"), 2, 0},
  {0x7B, D("\x04\x57"), 2, 0},
  {0x7E, D("\x01\x0E"), 2, 0},
  {0xBF, D("\x36"), 1, 0},
  {0xE3, D("\x40\x40"), 2, 0},
  {0xF0, D("\x00"), 1, 0},
  {0xD0, D("\x00"), 1, 0},
  {0x2A, D("\x00\x00\x01\x3F"), 4, 0},
  {0x2B, D("\x00\x00\x01\xDF"), 4, 0},
  {0x21, nullptr, 0, 0},
  {0x11, nullptr, 0, 120},
  {0x29, nullptr, 0, 0},
  {0x2C, nullptr, 0, 0},
  {0x3A, D("\x01"), 1, 0},
  {0x36, D("\x00"), 1, 0},
  {0x35, D("\x01"), 1, 20},
};

/* ---------- шина ---------- */
static spi_device_handle_t s_dev = nullptr;
static uint32_t s_hz = 80000000;
static uint8_t  s_rot = 1;

/*  Шматки для виводу кадру: поки один іде шиною, у другий повертається наступний.
    Шматок — цілі рядки панелі (≥ 8 × 320 пікселів), інакше розгортка обжене запис.
    Беремо їх зі спільного буфера смуг (lcdScratch, 16 КБ внутрішньої DMA-пам'яті):
    кадр виводиться, коли малювання вже скінчилось, тож буфер вільний. DMA SPI цієї
    версії IDF з PSRAM не бере («external memory is not supported now»), а окремі
    10 КБ внутрішньої пам'яті забрали б її в логотипів і TLS.  */
/*  16 стовпців × 320: кожен рядок кадру в PSRAM читається цілим рядком кешу (32 байти).
    З 8 стовпцями брали пів рядка — поворот усього екрана йшов ~12 мс (197 мкс на шмат),
    довше за саму шину (7,7 мс), і кадр зсуву не встигав у прохід розгортки.  */
static const size_t PB_PX = 5120;                  /* 10 КБ; два — 20 КБ спільного буфера */
static uint16_t* s_pb[2] = { nullptr, nullptr };
static uint8_t   s_k = 0;
/*  Черга транзакцій: окно (2 малі) + шматки пікселів. Результати приходять по
    черзі, тож рахуємо, скільки транзакцій у черзі й чиї вони.  */
static const int QN = 8;
static spi_transaction_t s_trWin[2][2];            /* CASET/RASET для двох вікон поспіль */
static uint8_t           s_win = 0;
static bool              s_busyWin[2] = { false, false };
static spi_transaction_t s_trPx[2];                /* пікселі з s_pb[k] */
static int               s_inq = 0;                /* транзакцій у черзі */
static bool              s_busyTb[2] = { false, false };

/*  Статистика (команда lcdstat)  */
static uint32_t s_stBytes = 0, s_stPieces = 0, s_stStrips = 0, s_stRotUs = 0, s_stWaitUs = 0, s_stScalar = 0;
static uint32_t s_stFrames = 0, s_stPresUs = 0, s_stPresMax = 0, s_stTeUs = 0, s_stTeMiss = 0, s_stFull = 0, s_stSplit = 0;
static bool     s_pie = true;

/*  Кадр у PSRAM (ландшафт 480×320, звичайний порядок байтів) — усе, що є на
    екрані. Він же «тінь» для знімків. Змінена частина — прямокутник [x0,x1)×[y0,y1).  */
static uint16_t* s_fb = nullptr;
static int16_t   s_bx0 = DSP_W, s_by0 = DSP_H, s_bx1 = 0, s_by1 = 0;
/*  Змінені прямокутники кадру (смуги з lcdPush): дрібні виводяться кожен своїм
    вікном (головний екран: риски спектра внизу, годинник посередині, значки вгорі —
    об'єднані в один прямокутник, вони тягли б пів екрана), великі — одним вікном.  */
struct DRect { int16_t x0, y0, x1, y1; };
static const uint8_t DR_N = 16;
static DRect   s_dr[DR_N];
static uint8_t s_drN = 0;
static bool    s_drOver = false;                    /* не вмістились — лише загальний прямокутник */
static SemaphoreHandle_t s_presMux = nullptr;       /* вивід кадру — з однієї задачі за раз */

/*  Сигнал TE: панель позначає ним гасіння між проходами розгортки.  */
static SemaphoreHandle_t s_teSem = nullptr;
static volatile int64_t  s_teRise = 0, s_teFall = 0, s_teLast = 0;
static volatile int32_t  s_teHi = 0, s_teLo = 0;
static volatile uint32_t s_teEdges = 0;
static volatile uint8_t  s_teEdge = 0;              /* 0 — без такту, 1 — гасіння з наростання, 2 — зі спаду */
static uint8_t           s_teAuto = 0;              /* що вибрав замір при старті */

static void addDevice(uint32_t hz){
  if(s_dev){ spi_bus_remove_device(s_dev); s_dev = nullptr; }
  spi_device_interface_config_t dc = {};
  dc.command_bits = 8;
  dc.address_bits = 24;
  dc.mode = 0;
  dc.clock_speed_hz = (int)hz;
  dc.spics_io_num = TFT_CS;
  dc.flags = SPI_DEVICE_HALFDUPLEX;
  dc.queue_size = QN;
  if(spi_bus_add_device(SPI2_HOST, &dc, &s_dev) != ESP_OK){ s_dev = nullptr; Serial.println("##[ERROR]#\tST77922: пристрій на шині не додано"); }
  s_hz = hz;
}

/*  Забрати з черги одну готову транзакцію (і звільнити її буфер, якщо це пікселі).  */
static void popOne(){
  spi_transaction_t* r = nullptr;
  if(spi_device_get_trans_result(s_dev, &r, pdMS_TO_TICKS(500)) != ESP_OK){
    Serial.println("##DSP#\tST77922: транзакція не завершилась за 500 мс");
    s_inq = 0; s_busyTb[0] = s_busyTb[1] = false; s_busyWin[0] = s_busyWin[1] = false;
    return;
  }
  s_inq--;
  if(r == &s_trPx[0]) s_busyTb[0] = false;
  else if(r == &s_trPx[1]) s_busyTb[1] = false;
  else if(r == &s_trWin[0][1]) s_busyWin[0] = false;
  else if(r == &s_trWin[1][1]) s_busyWin[1] = false;
}

static void drain(){ while(s_inq > 0) popOne(); }

/*  Регістр — лише коли черга порожня (опитувальна транзакція не змішується з чергою).  */
static void cmd(uint8_t c, const uint8_t* data, uint8_t len){
  if(!s_dev) return;
  if(s_presMux) xSemaphoreTake(s_presMux, portMAX_DELAY);
  drain();
  spi_transaction_t t = {};
  t.cmd = 0x02;
  t.addr = (uint32_t)c << 8;
  if(len && len <= 4){ t.flags = SPI_TRANS_USE_TXDATA; memcpy(t.tx_data, data, len); }
  else t.tx_buffer = data;
  t.length = (size_t)len * 8;
  spi_device_polling_transmit(s_dev, &t);
  if(s_presMux) xSemaphoreGive(s_presMux);
}

/*  Читання регістра (код 0x0B, по одній лінії, dummy — тактів очікування). Лише для
    перевірок (scanline): черга мусить бути порожня.  */
static void rd(uint8_t c, uint8_t* out, uint8_t len, uint8_t dummy){
  spi_transaction_ext_t t = {};
  t.base.flags = SPI_TRANS_VARIABLE_DUMMY;
  t.base.cmd = 0x0B;
  t.base.addr = (uint32_t)c << 8;
  t.dummy_bits = dummy;
  t.base.length = 0;
  t.base.rxlength = (size_t)len * 8;
  t.base.rx_buffer = out;
  spi_device_polling_transmit(s_dev, (spi_transaction_t*)&t);
}

/*  Вікно в системі панелі — у чергу (дві короткі транзакції з даними всередині)  */
static void queueWindow(int16_t px, int16_t py, int16_t w, int16_t h){
  spi_transaction_t* t = s_trWin[s_win];
  /*  Набір із двох транзакцій можна переписати, лише коли обидві вже пройшли
      (інакше зіпсуємо те, що DMA ще не відіслав).  */
  while(s_busyWin[s_win] && s_inq > 0) popOne();
  while(s_inq > QN - 2) popOne();
  memset(t, 0, sizeof(spi_transaction_t) * 2);
  t[0].cmd = 0x02; t[0].addr = 0x2A << 8; t[0].flags = SPI_TRANS_USE_TXDATA; t[0].length = 32;
  t[0].tx_data[0] = px >> 8; t[0].tx_data[1] = px; t[0].tx_data[2] = (px + w - 1) >> 8; t[0].tx_data[3] = px + w - 1;
  t[1].cmd = 0x02; t[1].addr = 0x2B << 8; t[1].flags = SPI_TRANS_USE_TXDATA; t[1].length = 32;
  t[1].tx_data[0] = py >> 8; t[1].tx_data[1] = py; t[1].tx_data[2] = (py + h - 1) >> 8; t[1].tx_data[3] = py + h - 1;
  spi_device_queue_trans(s_dev, &t[0], portMAX_DELAY); s_inq++;
  spi_device_queue_trans(s_dev, &t[1], portMAX_DELAY); s_inq++;
  s_busyWin[s_win] = true;
  s_win ^= 1;
}

static void queuePixels(uint8_t k, size_t n, bool first){
  spi_transaction_t& t = s_trPx[k];
  memset(&t, 0, sizeof(t));
  t.flags = SPI_TRANS_MODE_QIO;
  t.cmd = 0x32;
  t.addr = (first ? 0x2C : 0x3C) << 8;
  t.tx_buffer = s_pb[k];
  t.length = n * 16;
  s_busyTb[k] = true;
  if(spi_device_queue_trans(s_dev, &t, portMAX_DELAY) != ESP_OK){ s_busyTb[k] = false; return; }
  s_inq++;
  s_stBytes += n * 2; s_stPieces++;
}


/*  Дочекатись, поки буфер k звільниться  */
static void waitTb(uint8_t k){
  if(!s_busyTb[k]) return;
  int64_t t0 = esp_timer_get_time();
  while(s_busyTb[k] && s_inq > 0) popOne();
  s_stWaitUs += (uint32_t)(esp_timer_get_time() - t0);
}

/* ---------- поворот смуги ---------- */
/*  Блок 8×8 пікселів: 8 рядків джерела з кроком sStride байтів (може бути
    від'ємним) → 8 рядків призначення з кроком dStride, байти переставлено
    в порядок дисплея. Схему перестановки 8×8 трьома раундами ee.vzip.16 по
    парах (0,4)(1,5)(2,6)(3,7) → (0,2)(4,6)(1,3)(5,7) → (0,1)(2,3)(4,5)(6,7)
    перевірено моделлю; на платі — командою rotcheck. Після неї стовпчик c у qc.
    Перестановка байтів пари (qa,qb): vunzip.8 qa,qb; vzip.8 qb,qa дає
    qb = swap(стовпчик a), qa = swap(стовпчик b) — тому записи навхрест.  */
static inline void IRAM_ATTR rotBlockPie(const uint16_t* s, int32_t sStride, uint16_t* d, int32_t dStride){
  __asm__ volatile(
    "ee.vld.128.xp q0, %0, %2\n"
    "ee.vld.128.xp q1, %0, %2\n"
    "ee.vld.128.xp q2, %0, %2\n"
    "ee.vld.128.xp q3, %0, %2\n"
    "ee.vld.128.xp q4, %0, %2\n"
    "ee.vld.128.xp q5, %0, %2\n"
    "ee.vld.128.xp q6, %0, %2\n"
    "ee.vld.128.xp q7, %0, %2\n"
    "ee.vzip.16 q0, q4\n"
    "ee.vzip.16 q1, q5\n"
    "ee.vzip.16 q2, q6\n"
    "ee.vzip.16 q3, q7\n"
    "ee.vzip.16 q0, q2\n"
    "ee.vzip.16 q4, q6\n"
    "ee.vzip.16 q1, q3\n"
    "ee.vzip.16 q5, q7\n"
    "ee.vzip.16 q0, q1\n"
    "ee.vzip.16 q2, q3\n"
    "ee.vzip.16 q4, q5\n"
    "ee.vzip.16 q6, q7\n"
    "ee.vunzip.8 q0, q1\n"
    "ee.vzip.8   q1, q0\n"
    "ee.vunzip.8 q2, q3\n"
    "ee.vzip.8   q3, q2\n"
    "ee.vunzip.8 q4, q5\n"
    "ee.vzip.8   q5, q4\n"
    "ee.vunzip.8 q6, q7\n"
    "ee.vzip.8   q7, q6\n"
    "ee.vst.128.xp q1, %1, %3\n"
    "ee.vst.128.xp q0, %1, %3\n"
    "ee.vst.128.xp q3, %1, %3\n"
    "ee.vst.128.xp q2, %1, %3\n"
    "ee.vst.128.xp q5, %1, %3\n"
    "ee.vst.128.xp q4, %1, %3\n"
    "ee.vst.128.xp q7, %1, %3\n"
    "ee.vst.128.xp q6, %1, %3\n"
    : "+r"(s), "+r"(d) : "r"(sStride), "r"(dStride) : "memory");
}

/*  Повернути шмат смуги: стовпчики джерела [c0, c0+pc) (src — w×h, рядок за
    рядком) у dst, де кожен рядок панелі — h пікселів. Порядок рядків панелі —
    за зростанням py: для повороту 1 це стовпчики зліва направо, для 3 — справа наліво.  */
static void rotPiece(const uint16_t* src, int16_t w, int16_t h, int16_t c0, int16_t pc, uint16_t* dst, uint8_t rot, bool pie){
  const bool canPie = pie && !(h & 7) && !(c0 & 7) && !(pc & 7) && !(w & 7) &&
                      !(((uintptr_t)src | (uintptr_t)dst) & 15);
  if(canPie){
    for(int16_t cb = 0; cb < pc; cb += 8){
      for(int16_t rb = 0; rb < h; rb += 8){
        if(rot == 1){
          /*  стовпчик (c0+cb+i) → рядок панелі (cb+i); рядки джерела навпаки:
              блок rb іде в кінець рядка панелі (h − 8 − rb)  */
          const uint16_t* s = src + (int32_t)(rb + 7) * w + c0 + cb;
          uint16_t* d = dst + (int32_t)cb * h + (h - 8 - rb);
          rotBlockPie(s, -(int32_t)w * 2, d, (int32_t)h * 2);
        }else{
          /*  стовпчик (c0+cb+i) → рядок панелі (pc−1−cb−i); рядки — у прямому порядку  */
          const uint16_t* s = src + (int32_t)rb * w + c0 + cb;
          uint16_t* d = dst + (int32_t)(pc - 1 - cb) * h + rb;
          rotBlockPie(s, (int32_t)w * 2, d, -(int32_t)h * 2);
        }
      }
    }
    return;
  }
  s_stScalar++;
  for(int16_t i = 0; i < pc; i++){
    uint16_t* d = dst + (int32_t)i * h;
    if(rot == 1){
      const uint16_t* col = src + c0 + i + (int32_t)(h - 1) * w;
      for(int16_t j = 0; j < h; j++, col -= w){ uint16_t v = *col; d[j] = (uint16_t)((v >> 8) | (v << 8)); }
    }else{
      const uint16_t* col = src + c0 + (pc - 1 - i);
      for(int16_t j = 0; j < h; j++, col += w){ uint16_t v = *col; d[j] = (uint16_t)((v >> 8) | (v << 8)); }
    }
  }
}

/* ---------- такт панелі (TE) ---------- */
static void IRAM_ATTR teIsr(void*){
  const int64_t t = esp_timer_get_time();
  const uint32_t lvl = gpio_ll_get_level(&GPIO, (gpio_num_t)LCD_TE);
  s_teEdges++;
  BaseType_t woken = pdFALSE;
  if(lvl){
    s_teLo = (int32_t)(t - s_teFall); s_teRise = t;
    if(s_teEdge == 1){ s_teLast = t; xSemaphoreGiveFromISR(s_teSem, &woken); }
  }else{
    s_teHi = (int32_t)(t - s_teRise); s_teFall = t;
    if(s_teEdge == 2){ s_teLast = t; xSemaphoreGiveFromISR(s_teSem, &woken); }
  }
  if(woken) portYIELD_FROM_ISR();
}

/*  Увімкнути переривання TE і заміряти: яка фаза коротша — та й гасіння (за
    даними виробника: оновлення з пам'яті 13 мс, гасіння 3 мс). Якщо панель дає
    ще й імпульси кожного рядка (режим 1 команди 0x35), — лише кадрові (режим 0).  */
static void teBegin(){
#ifdef LCD_TE
  s_teSem = xSemaphoreCreateBinary();
  gpio_config_t c = {};
  c.pin_bit_mask = 1ULL << LCD_TE;
  c.mode = GPIO_MODE_INPUT;
  c.pull_up_en = GPIO_PULLUP_ENABLE;
  c.intr_type = GPIO_INTR_ANYEDGE;
  gpio_config(&c);
  esp_err_t e = gpio_install_isr_service(0);
  if(e != ESP_OK && e != ESP_ERR_INVALID_STATE){ Serial.printf("##DSP#\tTE: служба переривань — %s\n", esp_err_to_name(e)); return; }
  if(gpio_isr_handler_add((gpio_num_t)LCD_TE, teIsr, nullptr) != ESP_OK){ Serial.println("##DSP#\tTE: переривання не додано"); return; }
  s_teEdges = 0; delay(120);
  uint32_t n = s_teEdges;
  if(n > 60){
    static const uint8_t m0[] = { 0x00 };
    cmd(0x35, m0, 1);
    s_teEdges = 0; delay(120); n = s_teEdges;
    Serial.printf("##DSP#\tTE: були імпульси рядків — тепер лише кадрові (0x35 00)\n");
  }
  if(n < 4){ s_teEdge = 0; s_teAuto = 0; Serial.printf("##DSP#\tTE: сигналу немає (%u фронтів) — кадр без такту\n", (unsigned)n); return; }
  s_teAuto = (s_teHi > 0 && s_teLo > 0 && s_teHi < s_teLo) ? 1 : 2;
  s_teEdge = s_teAuto;
  Serial.printf("##DSP#\tTE: кадр %u мкс (високий %u, низький %u), гасіння — з %s\n",
                (unsigned)(s_teHi + s_teLo), (unsigned)s_teHi, (unsigned)s_teLo, s_teAuto == 1 ? "наростання" : "спаду");
#endif
}

/*  Дочекатись початку гасіння. Якщо воно щойно почалось (≤ 0,4 мс тому) — одразу.  */
static void waitBlank(){
  if(!s_teEdge || !s_teSem) return;
  const int64_t t0 = esp_timer_get_time();
  if(t0 - s_teLast < 400) return;
  xSemaphoreTake(s_teSem, 0);                       /* давній сигнал не рахується */
  if(xSemaphoreTake(s_teSem, pdMS_TO_TICKS(40)) != pdTRUE) s_stTeMiss++;
  s_stTeUs += (uint32_t)(esp_timer_get_time() - t0);
}

/*  Коли можна писати рядки панелі [r0, r1) (px точок), щоб розгортка не зловила
    запис посередині. Розгортка: гасіння vb мкс від наростання TE, далі рядки 0..479
    за sc мкс. Запис іде не повільніше за шину (px × 0,05 мкс) і швидший за розгортку,
    тож годиться будь-що з двох:
      «попереду» — почати, поки розгортка не дійшла до r0 (запис її не підпустить);
      «позаду»   — почати так пізно, щоб і останній рядок r1 ми записали вже після
                   розгортки (цей прохід покаже старе, наступний — нове).
    Заборонене лише вікно між ними. Раніше кадр чекав саме гасіння, і прокрутка
    (малювання 7 + вивід 7,6 мс) щоразу пропускала один прохід — 30 кадрів/с.  */
static uint32_t s_stAhead = 0, s_stBehind = 0, s_stWaitFrame = 0;
static void waitSafe(int16_t r0, int16_t r1, uint32_t px){
  if(!s_teEdge || !s_teSem){ return; }
  const int32_t P = s_teHi + s_teLo, vb = (s_teEdge == 1) ? s_teHi : s_teLo, sc = P - vb;
  if(P < 8000 || P > 40000){ waitBlank(); return; }
  const int32_t dMin = (int32_t)(px / 20);          /* шина: 20 млн точок на секунду */
  const int32_t M = 500;                            /* запас, мкс */
  const int64_t t0 = esp_timer_get_time();
  for(uint8_t guard = 0; guard < 4; guard++){
    const int64_t now = esp_timer_get_time();
    const int32_t tau = (int32_t)(now - s_teLast);
    if(tau < 0 || tau > P + 2000){ waitBlank(); break; }                 /* такт загубився — чекаємо наступний */
    const int32_t beam0 = vb + (int32_t)r0 * sc / PH, beam1 = vb + (int32_t)r1 * sc / PH;
    if(tau + M < beam0){ s_stAhead++; break; }                          /* попереду */
    if(tau + dMin > beam1 + M){ s_stBehind++; break; }                  /* позаду */
    /*  заборонене вікно: до «позаду» чи до наступного гасіння — що раніше  */
    const int32_t tb = beam1 + M - dMin + 50;
    const int32_t wait = tb < P ? tb - tau : P - tau;
    if(wait >= P - tau - 300){ s_stWaitFrame++; waitBlank(); break; }
    const int64_t until = now + wait;
    while(esp_timer_get_time() < until){ if(until - esp_timer_get_time() > 1500) vTaskDelay(1); }
  }
  s_stTeUs += (uint32_t)(esp_timer_get_time() - t0);
}

/* ---------- кадр у PSRAM → панель ---------- */
static inline void markDirty(int16_t x0, int16_t y0, int16_t x1, int16_t y1){
  if(x0 < s_bx0) s_bx0 = x0; if(y0 < s_by0) s_by0 = y0;
  if(x1 > s_bx1) s_bx1 = x1; if(y1 > s_by1) s_by1 = y1;
  if(s_drOver) return;
  /*  смуга, що продовжує попередню (той самий стовпчик, одразу нижче), — дописати до неї  */
  if(s_drN){
    DRect& l = s_dr[s_drN - 1];
    if(l.x0 == x0 && l.x1 == x1 && l.y1 == y0){ l.y1 = y1; return; }
  }
  if(s_drN < DR_N) s_dr[s_drN++] = { x0, y0, x1, y1 };
  else s_drOver = true;
}
static inline void clearDirty(){ s_bx0 = DSP_W; s_by0 = DSP_H; s_bx1 = 0; s_by1 = 0; s_drN = 0; s_drOver = false; }

/*  Другий лист — для переходу зсувом: туди малюється нова сторінка, поки на
    екрані (і в s_fb) ще прежня.  */
static uint16_t* s_fb2 = nullptr;
static uint16_t* s_tgt = nullptr;                    /* куди пише lcdPush: s_fb або s_fb2 */
static uint32_t  s_slFrames = 0, s_slMs = 0, s_slMaxUs = 0, s_slLate = 0;   /* останній зсув (команда lcdstat) */
static int16_t   s_slOff[40];                         /* зсуви по кадрах (команда slidetest) */
static uint16_t  s_slUs[40];

/*  Стовпці екрана [dx, dx+n) рядків [y0,y1) — зі стовпців [sx, sx+n) листа fb —
    в панель: одне вікно, шматки цілими рядками панелі в порядку розгортки.
    Шматок, що в листі не з межі 8 стовпців (зсув), повертається звичайним кодом,
    решта — PIE.  */
static void pushCols(const uint16_t* fb, int16_t sx, int16_t dx, int16_t n, int16_t y0, int16_t y1, uint8_t rot){
  if(n <= 0 || y1 <= y0) return;
  const int16_t h = y1 - y0;
  int16_t per = (int16_t)(PB_PX / h);
  per -= per & 7;
  if(per < 8) per = 8;                               /* h ≤ 320, PB_PX ≥ 8 × 320 — вміщається */
  const uint16_t* src = fb + (int32_t)y0 * DSP_W;    /* стовпці — абсолютні (вирівнювання PIE — за sx) */
  int16_t px0, py0;
  if(rot == 1){ px0 = PW - y1; py0 = dx; }
  else        { px0 = y0;      py0 = PH - dx - n; }
  queueWindow(px0, py0, h, n);
  s_stStrips++;
  bool first = true;
  /*  поворот 1: рядки панелі за зростанням — стовпці зліва направо; 3 — справа наліво  */
  int16_t cur = rot == 1 ? sx : sx + n;
  const int16_t lo = sx, hi = sx + n;
  while(rot == 1 ? cur < hi : cur > lo){
    int16_t pc, c0;
    if(rot == 1){
      const int16_t left = hi - cur;
      if(cur & 7) pc = 8 - (cur & 7) < left ? 8 - (cur & 7) : left;          /* до межі 8 */
      else{ pc = left < per ? left : per; if(pc >= 8) pc -= pc & 7; }
      c0 = cur; cur += pc;
    }else{
      const int16_t left = cur - lo;
      if(cur & 7) pc = (cur & 7) < left ? (cur & 7) : left;
      else{ pc = left < per ? left : per; if(pc >= 8) pc -= pc & 7; }
      cur -= pc; c0 = cur;
    }
    const uint8_t k = s_k;
    waitTb(k);
    const int64_t r0 = esp_timer_get_time();
    rotPiece(src, DSP_W, h, c0, pc, s_pb[k], rot, s_pie);
    s_stRotUs += (uint32_t)(esp_timer_get_time() - r0);
    queuePixels(k, (size_t)pc * h, first);
    first = false;
    s_k ^= 1;
  }
}

/*  Змінену частину кадру — в панель з початку гасіння.  */
static void present(){
  if(!s_dev || !s_fb) return;
  if(s_tgt == s_fb2 && s_fb2) return;                /* готується лист переходу — виводить lcdSlideRun */
  if(s_bx1 <= s_bx0 || s_by1 <= s_by0) return;
  uint16_t* sc = (uint16_t*)lcdScratch(PB_PX * 2 * 2);
  if(!sc) return;
  if(s_presMux) xSemaphoreTake(s_presMux, portMAX_DELAY);
  s_pb[0] = sc; s_pb[1] = sc + PB_PX;
  const uint8_t rot = s_rot;
  /*  стовпці — до 8 (PIE і вирівнювання 16 байтів), рядки — до 8 (стовпці вікна панелі кратні 4)  */
  auto al = [](DRect r){
    r.x0 &= ~7; r.x1 = (r.x1 + 7) & ~7; r.y0 &= ~7; r.y1 = (r.y1 + 7) & ~7;
    if(r.x1 > DSP_W) r.x1 = DSP_W; if(r.y1 > DSP_H) r.y1 = DSP_H;
    return r;
  };
  DRect bb = al({ s_bx0, s_by0, s_bx1, s_by1 });
  DRect list[DR_N]; uint8_t n = 0;
  uint32_t sum = 0;
  if(!s_drOver) for(uint8_t i = 0; i < s_drN; i++){ list[n] = al(s_dr[i]); sum += (uint32_t)(list[n].x1 - list[n].x0) * (list[n].y1 - list[n].y0); n++; }
  clearDirty();
  /*  Окремими вікнами — лише коли все вміститься в гасіння (3,3 мс; ~40 КБ з поворотом):
      тоді порядок неважливий. Інакше — одним вікном у порядку розгортки.  */
  const bool split = n > 1 && sum <= 20000 && sum * 2 < (uint32_t)(bb.x1 - bb.x0) * (bb.y1 - bb.y0);
  /*  рядки панелі вікна: поворот 1 — стовпці x, 3 — дзеркально  */
  const int16_t r0 = rot == 1 ? bb.x0 : PH - bb.x1, r1 = rot == 1 ? bb.x1 : PH - bb.x0;
  waitSafe(r0, r1, split ? sum : (uint32_t)(bb.x1 - bb.x0) * (bb.y1 - bb.y0));
  const int64_t t0 = esp_timer_get_time();
  if(split){
    /*  за зростанням рядка панелі (поворот 1 — x, 3 — навпаки)  */
    for(uint8_t i = 1; i < n; i++) for(uint8_t j = i; j > 0 && (rot == 1 ? list[j].x0 < list[j - 1].x0 : list[j].x1 > list[j - 1].x1); j--){ DRect t = list[j]; list[j] = list[j - 1]; list[j - 1] = t; }
    for(uint8_t i = 0; i < n; i++) pushCols(s_fb, list[i].x0, list[i].x0, list[i].x1 - list[i].x0, list[i].y0, list[i].y1, rot);
    s_stSplit++;
  }else pushCols(s_fb, bb.x0, bb.x0, bb.x1 - bb.x0, bb.y0, bb.y1, rot);
  drain();
  const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
  s_stFrames++; s_stPresUs += us; if(us > s_stPresMax) s_stPresMax = us;
  if(bb.x1 - bb.x0 == DSP_W && bb.y1 - bb.y0 >= DSP_H - 64 && !split) s_stFull++;
  if(s_presMux) xSemaphoreGive(s_presMux);
}

/* ---------- перехід зсувом ---------- */
bool lcdSlideBegin(){
  if(!s_fb) return false;
  if(!s_fb2) s_fb2 = (uint16_t*)heap_caps_aligned_alloc(16, (size_t)DSP_W * DSP_H * 2, MALLOC_CAP_SPIRAM);
  if(!s_fb2) return false;
  present();                                         /* що лишилось від прежньої сторінки — на екран */
  s_tgt = s_fb2;
  return true;
}

/*  Як у передатчику (hearing-link, txui.h slideTo): «швидко пішла, м'яко стала» —
    крок за кривою 1 − (1 − t)³; на кожен такт панелі — один кадр, складений зі
    стовпців двох листів. Сцена не малюється — кадр стоїть лише за шиною (~8 мс),
    тож кожен прохід розгортки показує новий зсув.  */
void lcdSlideRun(int8_t dir, uint32_t ms){
  if(!s_fb2 || s_tgt != s_fb2){ s_tgt = s_fb; return; }
  uint16_t* sc = (uint16_t*)lcdScratch(PB_PX * 2 * 2);
  if(!sc || !s_dev){ s_tgt = s_fb; uint16_t* t = s_fb; s_fb = s_fb2; s_fb2 = t; markDirty(0, 0, DSP_W, DSP_H); present(); return; }
  if(s_presMux) xSemaphoreTake(s_presMux, portMAX_DELAY);
  s_pb[0] = sc; s_pb[1] = sc + PB_PX;
  const uint8_t rot = s_rot;
  const uint32_t per = (s_teEdge && s_teHi + s_teLo > 8000) ? (uint32_t)(s_teHi + s_teLo) : 16000;
  int n = (int)((ms * 1000 + per / 2) / per);
  if(n < 6) n = 6;
  if(n > 40) n = 40;
  const uint32_t t0 = millis();
  s_slMaxUs = 0; s_slLate = 0;
  int64_t prev = esp_timer_get_time();
  const int64_t start = prev;
  for(int i = 1; i <= n; i++){
    const float t = 1.0f - (float)i / n;
    const int16_t off = i == n ? DSP_W : (int16_t)lroundf(DSP_W * (1.0f - t * t * t));
    if(s_teEdge) waitBlank();
    else{ const int64_t due = start + (int64_t)i * per; while(esp_timer_get_time() < due) vTaskDelay(1); }   /* без TE — рівний темп */
    const int64_t u0 = esp_timer_get_time();
    if(dir > 0){
      /*  новий в'їжджає справа: зліва — прежній від стовпця off, справа — початок нового  */
      if(rot == 1){ pushCols(s_fb, off, 0, DSP_W - off, 0, DSP_H, rot); pushCols(s_fb2, 0, DSP_W - off, off, 0, DSP_H, rot); }
      else        { pushCols(s_fb2, 0, DSP_W - off, off, 0, DSP_H, rot); pushCols(s_fb, off, 0, DSP_W - off, 0, DSP_H, rot); }
    }else{
      /*  новий в'їжджає зліва: зліва — кінець нового, справа — прежній  */
      if(rot == 1){ pushCols(s_fb2, DSP_W - off, 0, off, 0, DSP_H, rot); pushCols(s_fb, 0, off, DSP_W - off, 0, DSP_H, rot); }
      else        { pushCols(s_fb, 0, off, DSP_W - off, 0, DSP_H, rot); pushCols(s_fb2, DSP_W - off, 0, off, 0, DSP_H, rot); }
    }
    drain();
    const int64_t u1 = esp_timer_get_time();
    s_slOff[i - 1] = off; s_slUs[i - 1] = (uint16_t)((u1 - u0) > 65535 ? 65535 : (u1 - u0));
    if((uint32_t)(u1 - u0) > s_slMaxUs) s_slMaxUs = (uint32_t)(u1 - u0);
    if(i > 1 && u1 - prev > (int64_t)per * 3 / 2) s_slLate++;     /* пропустили прохід розгортки */
    prev = u1;
  }
  s_slFrames = n; s_slMs = millis() - t0;
  /*  новий лист — тепер кадр  */
  uint16_t* t = s_fb; s_fb = s_fb2; s_fb2 = t;
  s_tgt = s_fb;
  clearDirty();
  if(s_presMux) xSemaphoreGive(s_presMux);
}

/*  Сирий регістр (команда lcdrd <рег> <байтів> <тактів очікування>)  */
void lcdRegRead(uint8_t reg, uint8_t len, uint8_t dummy){
  if(!s_dev || len > 8) return;
  if(s_presMux) xSemaphoreTake(s_presMux, portMAX_DELAY);
  drain();
  uint8_t b[8] = { 0 };
  rd(reg, b, len, dummy);
  if(s_presMux) xSemaphoreGive(s_presMux);
  Serial.printf("LCDRD 0x%02X (очікування %u):", reg, dummy);
  for(uint8_t i = 0; i < len; i++) Serial.printf(" %02X", b[i]);
  Serial.println();
}

/*  Перевірка зсуву (команда slidetest 1|-1): той самий екран їде сам на себе —
    зсуви й час кожного кадру, скільки шматків пішло звичайним кодом.  */
void lcdSlideTest(int8_t dir){
  if(!lcdSlideBegin()){ Serial.println("SLIDETEST: немає другого листа"); return; }
  memcpy(s_fb2, s_fb, (size_t)DSP_W * DSP_H * 2);
  const uint32_t sc0 = s_stScalar, p0 = s_stPieces;
  lcdSlideRun(dir, 240);
  Serial.printf("SLIDETEST кадрів %u за %u мс, найдовший %u мкс, пропущено проходів %u, шматків %u, з них звичайним кодом %u\n",
                (unsigned)s_slFrames, (unsigned)s_slMs, (unsigned)s_slMaxUs, (unsigned)s_slLate,
                (unsigned)(s_stPieces - p0), (unsigned)(s_stScalar - sc0));
  for(uint32_t i = 0; i < s_slFrames && i < 40; i++) Serial.printf("  кадр %2u: зсув %3d, %5u мкс\n", (unsigned)(i + 1), s_slOff[i], (unsigned)s_slUs[i]);
}

/* ---------- lcdout для ST77922 ---------- */
void* lcdScratch(size_t bytes){ return spidmaScratch(bytes); }
void  lcdBegin(){}
void  lcdWait(){}                 /* смуги копіюються в кадр одразу — чекати нічого */
void  lcdEnd(){ present(); }

/*  Малювати прямо в кадр (або в лист переходу) — без копії смуги.  */
uint16_t* lcdFrameFor(int16_t x, int16_t y){
  uint16_t* fb = s_tgt ? s_tgt : s_fb;
  if(!fb || x < 0 || y < 0 || x >= DSP_W || y >= DSP_H) return nullptr;
  return fb + (int32_t)y * DSP_W + x;
}
int16_t lcdFrameStride(){ return DSP_W; }
void lcdMarked(int16_t x, int16_t y, int16_t w, int16_t h){
  if(w <= 0 || h <= 0) return;
  if((s_tgt ? s_tgt : s_fb) == s_fb) markDirty(x, y, x + w, y + h);
}

/*  Смуга → кадр у PSRAM (або в лист переходу). Буфер смуги вільний одразу.  */
bool lcdPush(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t* buf){
  uint16_t* fb = s_tgt ? s_tgt : s_fb;
  if(!fb || w <= 0 || h <= 0) return false;
  if(x < 0 || y < 0 || x + w > DSP_W || y + h > DSP_H) return false;
  for(int16_t r = 0; r < h; r++) memcpy(fb + (int32_t)(y + r) * DSP_W + x, buf + (int32_t)r * w, (size_t)w * 2);
  if(fb == s_fb) markDirty(x, y, x + w, y + h);
  return false;
}

/* ---------- службове ---------- */
/*  «Тінь» тепер — сам кадр у PSRAM: є завжди.  */
void lcdShadow(bool on){ (void)on; }
bool lcdShadowOn(){ return s_fb != nullptr; }
uint8_t st77922Rot(){ return s_rot; }
uint32_t st77922Hz(){ return s_hz; }
void lcdRotPie(int8_t on){ if(on >= 0) s_pie = on != 0; Serial.printf("ROTPIE %d (поворот %s)\n", s_pie ? 1 : 0, s_pie ? "векторний PIE" : "звичайним кодом"); }

void lcdStats(bool reset){
  Serial.printf("LCDSTAT ST77922 %u МГц, поворот %u, кадрів %u (повних %u, окремими вікнами %u), шматків %u, %u КБ, поворот %u мкс (звичайним кодом %u шматків), очікування шини %u мкс, PIE %d\n",
                (unsigned)(s_hz / 1000000), s_rot, (unsigned)s_stFrames, (unsigned)s_stFull, (unsigned)s_stSplit, (unsigned)s_stPieces, (unsigned)(s_stBytes / 1024),
                (unsigned)s_stRotUs, (unsigned)s_stScalar, (unsigned)s_stWaitUs, s_pie ? 1 : 0);
  Serial.printf("LCDSTAT вивід кадру: в середньому %u мкс, найдовший %u мкс; чекання гасіння %u мкс, без сигналу %u разів; TE: %s, кадр %u мкс (високий %u, низький %u), фронтів %u\n",
                (unsigned)(s_stFrames ? s_stPresUs / s_stFrames : 0), (unsigned)s_stPresMax, (unsigned)s_stTeUs, (unsigned)s_stTeMiss,
                s_teEdge == 1 ? "наростання" : s_teEdge == 2 ? "спад" : "вимкнено",
                (unsigned)(s_teHi + s_teLo), (unsigned)s_teHi, (unsigned)s_teLo, (unsigned)s_teEdges);
  Serial.printf("LCDSTAT початок виводу: попереду розгортки %u, позаду %u, до наступного гасіння %u\n",
                (unsigned)s_stAhead, (unsigned)s_stBehind, (unsigned)s_stWaitFrame);
  Serial.printf("LCDSTAT останній зсув: кадрів %u за %u мс, найдовший кадр %u мкс, пропущених проходів розгортки %u\n",
                (unsigned)s_slFrames, (unsigned)s_slMs, (unsigned)s_slMaxUs, (unsigned)s_slLate);
  if(reset) s_stBytes = s_stPieces = s_stStrips = s_stRotUs = s_stWaitUs = s_stScalar = s_stFrames = s_stPresUs = s_stPresMax = s_stTeUs = s_stTeMiss = s_stFull = s_stSplit = s_stAhead = s_stBehind = s_stWaitFrame = 0;
}

/*  Такт панелі (команда te): -1 показати, 0 вимкнути, 1 наростання, 2 спад, 3 як заміряно при старті.  */
void lcdTe(int8_t mode){
  if(mode == 3) s_teEdge = s_teAuto;
  else if(mode >= 0 && mode <= 2) s_teEdge = (uint8_t)mode;
  Serial.printf("TE %u (%s; заміряно при старті: %s)\n", (unsigned)s_teEdge,
                s_teEdge == 1 ? "гасіння з наростання" : s_teEdge == 2 ? "гасіння зі спаду" : "без такту",
                s_teAuto == 1 ? "наростання" : s_teAuto == 2 ? "спад" : "сигналу немає");
}

/*  Звірка: випадкові смуги, обидва повороти, обидві реалізації — побайтно,
    і час кожної на смузі 480×8.  */
bool lcdRotCheck(){
  const int16_t W = 480, H = 16;
  /*  Буфери перевірки — у PSRAM: внутрішньої пам'яті обмаль (перша версія брала
      звідти 45 КБ і опускала мінімум вільної до 3 КБ). Час замірюється окремо
      на внутрішніх буферах смуги, як у роботі.  */
  uint16_t* src = (uint16_t*)heap_caps_aligned_alloc(16, (size_t)W * H * 2, MALLOC_CAP_SPIRAM);
  uint16_t* a   = (uint16_t*)heap_caps_aligned_alloc(16, (size_t)W * H * 2, MALLOC_CAP_SPIRAM);
  uint16_t* b   = (uint16_t*)heap_caps_aligned_alloc(16, (size_t)W * H * 2, MALLOC_CAP_SPIRAM);
  if(!src || !a || !b){ Serial.println("ROTCHECK: немає пам'яті"); heap_caps_free(src); heap_caps_free(a); heap_caps_free(b); return false; }
  uint32_t bad = 0, tests = 0;
  for(int i = 0; i < W * H; i++) src[i] = (uint16_t)esp_random();
  static const int16_t hs[] = { 8, 16 };
  static const int16_t ws[] = { 480, 320, 64, 16 };
  for(uint8_t rot = 1; rot <= 3; rot += 2)
    for(int16_t h : hs)
      for(int16_t w : ws)
        for(int16_t c0 = 0; c0 < w; c0 += (w > 64 ? 120 : 8)){
          int16_t pc = w - c0; if(pc > 128) pc = 128; pc -= pc & 7; if(pc <= 0) continue;
          memset(a, 0x55, (size_t)pc * h * 2); memset(b, 0xAA, (size_t)pc * h * 2);
          rotPiece(src, w, h, c0, pc, a, rot, false);
          rotPiece(src, w, h, c0, pc, b, rot, true);
          tests++;
          if(memcmp(a, b, (size_t)pc * h * 2)){ if(bad < 4) Serial.printf("ROTCHECK розбіжність: rot %u w %d h %d c0 %d pc %d\n", rot, w, h, c0, pc); bad++; }
        }
  /*  час — як у роботі: смуга у спільному внутрішньому буфері (lcdScratch), шмат
      у буфер повороту; сто повторів  */
  uint32_t tS = 0, tP = 0;
  /*  власні малі буфери (11,5 КБ на мить): спільний буфер смуги й буфери повороту
      в цю мить може брати задача екрана  */
  uint16_t* tsb = (uint16_t*)heap_caps_aligned_alloc(16, (size_t)480 * 8 * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  uint16_t* tdb = (uint16_t*)heap_caps_aligned_alloc(16, (size_t)256 * 8 * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if(tsb && tdb){
    memcpy(tsb, src, (size_t)480 * 8 * 2);
    { int64_t t0 = esp_timer_get_time(); for(int i = 0; i < 100; i++) rotPiece(tsb, 480, 8, 0, 256, tdb, 1, false); tS = (uint32_t)(esp_timer_get_time() - t0); }
    { int64_t t0 = esp_timer_get_time(); for(int i = 0; i < 100; i++) rotPiece(tsb, 480, 8, 0, 256, tdb, 1, true);  tP = (uint32_t)(esp_timer_get_time() - t0); }
  }
  heap_caps_free(tsb); heap_caps_free(tdb);
  s_stScalar = 0;
  Serial.printf("ROTCHECK %u перевірок, розбіжностей %u -> %s; шмат 256×8: звичайний код %u.%02u мкс, PIE %u.%02u мкс (у %u.%u раза швидше)\n",
                (unsigned)tests, (unsigned)bad, bad ? "ПОМИЛКА" : "ЗБІГАЄТЬСЯ",
                (unsigned)(tS / 100), (unsigned)(tS % 100), (unsigned)(tP / 100), (unsigned)(tP % 100),
                (unsigned)(tP ? tS / tP : 0), (unsigned)(tP ? (tS * 10 / tP) % 10 : 0));
  heap_caps_free(src); heap_caps_free(a); heap_caps_free(b);
  return bad == 0;
}

/* ---------- DspCore ---------- */
DspCore::DspCore() : Adafruit_GFX(DSP_W, DSP_H) {}

void DspCore::initDisplay(){
  spi_bus_config_t bc = {};
  bc.data0_io_num = LCD_QSPI_D0; bc.data1_io_num = LCD_QSPI_D1; bc.sclk_io_num = LCD_QSPI_SCK;
  bc.data2_io_num = LCD_QSPI_D2; bc.data3_io_num = LCD_QSPI_D3;
  bc.data4_io_num = -1; bc.data5_io_num = -1; bc.data6_io_num = -1; bc.data7_io_num = -1;
  bc.max_transfer_sz = PB_PX * 2 + 64;
  bc.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD | SPICOMMON_BUSFLAG_IOMUX_PINS;
  esp_err_t e = spi_bus_initialize(SPI2_HOST, &bc, SPI_DMA_CH_AUTO);
  if(e != ESP_OK) Serial.printf("##[ERROR]#\tST77922: шина QSPI — %s\n", esp_err_to_name(e));
  addDevice(s_hz);
  /*  кадр — у PSRAM, вирівняний на 16 байтів (векторне читання при повороті)  */
  s_fb = (uint16_t*)heap_caps_aligned_calloc(16, (size_t)DSP_W * DSP_H, 2, MALLOC_CAP_SPIRAM);
  if(!s_fb) Serial.println("##[ERROR]#\tST77922: немає пам'яті під кадр у PSRAM");
  s_tgt = s_fb;
  for(size_t i = 0; i < sizeof(INIT) / sizeof(INIT[0]); i++){
    cmd(INIT[i].cmd, INIT[i].data, INIT[i].len);
    if(INIT[i].delay_ms) delay(INIT[i].delay_ms);
  }
  s_presMux = xSemaphoreCreateMutex();
  teBegin();
  flip();
  invert();
  setTextWrap(false);
}

void DspCore::clearDsp(bool black){ fillScreen(black ? 0 : config.theme.background); }

/*  Заливка в екранних координатах: прямокутник панелі, стовпці вирівняно на 4.  */
/*  Заливка: у кадр і одразу в панель.  */
void DspCore::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color){
  if(!s_fb) return;
  if(x < 0){ w += x; x = 0; } if(y < 0){ h += y; y = 0; }
  if(x + w > DSP_W) w = DSP_W - x; if(y + h > DSP_H) h = DSP_H - y;
  if(w <= 0 || h <= 0) return;
  for(int16_t r = y; r < y + h; r++) m2::sFill(s_fb + (int32_t)r * DSP_W + x, (uint32_t)w, color);
  markDirty(x, y, x + w, y + h);
  present();
}

void DspCore::fillScreen(uint16_t color){ fillRect(0, 0, DSP_W, DSP_H, color); }
void DspCore::drawPixel(int16_t x, int16_t y, uint16_t color){ fillRect(x, y, 1, 1, color); }

/*  Логотип (240×76) посередині: рядки смуги кратні 8 — top з конфігурації такий.  */
void DspCore::drawLogo(uint16_t top){
  const int16_t w = (LOGO_WIDTH + 7) & ~7, h = (LOGO_HEIGHT + 7) & ~7;
  uint16_t* b = (uint16_t*)heap_caps_aligned_alloc(16, (size_t)w * h * 2, MALLOC_CAP_SPIRAM);   /* 240×80 — у PSRAM */
  if(!b) return;
  const uint16_t bg = config.theme.background;
  for(int16_t r = 0; r < h; r++)
    for(int16_t c = 0; c < w; c++)
      b[(int32_t)r * w + c] = (r < LOGO_HEIGHT && c < LOGO_WIDTH) ? pgm_read_word(&logo[r * LOGO_WIDTH + c]) : bg;
  int16_t x = ((DSP_W - LOGO_WIDTH) / 2) & ~7, y = top & ~7;
  lcdPush(x, y, w, h, b);
  present();
  heap_caps_free(b);
}

void DspCore::flip(){
  s_rot = config.store.flipscreen ? 3 : 1;
  markDirty(0, 0, DSP_W, DSP_H);           /* увесь кадр — у новому повороті (з наступним виводом) */
}
void DspCore::invert(){ cmd((config.store.invertdisplay ^ DSP_INVERT_BASE) ? 0x21 : 0x20, nullptr, 0); }
void DspCore::sleep(){ cmd(0x28, nullptr, 0); delay(20); cmd(0x10, nullptr, 0); delay(120); }
void DspCore::wake(){ cmd(0x11, nullptr, 0); delay(120); cmd(0x29, nullptr, 0); delay(20); }

void DspCore::setSPISpeed(uint32_t hz){ drain(); addDevice(hz); }

void DspCore::dbgReadRow(uint16_t y, uint16_t w, uint16_t* out){
  if(w > DSP_W) w = DSP_W;
  if(!s_fb || y >= DSP_H){ memset(out, 0, (size_t)w * 2); return; }
  memcpy(out, s_fb + (int32_t)y * DSP_W, (size_t)w * 2);
}

#endif  // DSP_MODEL==DSP_ST77922
