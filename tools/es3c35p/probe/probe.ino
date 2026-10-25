/*  ---------------------------------------------------------------------------
 *  Пробна прошивка для плати ES3C35P (LCDwiki 3.5" ESP32-S3, ST77922 QSPI).
 *  Перевіряє вузли плати по одному, перш ніж переносити на неї ПОТУЖНЕ РАДІО:
 *  екран (ініціалізація виробника, поворот, читання відеопам'яті), сенсор
 *  ST77922 (I2C 0x55), кодек ES8311 + підсилювач + мікрофон, картка SDIO,
 *  світлодіод WS2812, батарея.
 *
 *  Команди в послідовний порт (по рядку):
 *    info | i2c | lcd | pat <r> | rid | rd <x> <y> <w> | col <x> | madctl <hex> | spi <МГц>
 *    tp | tlog <сек> | tone [сек] | sd | bat | led <r> <g> <b> | te | heap
 *    intw <сек> | slp <1|2> <сек> | lsleep <1|2> <сек>   (1 — DISPOFF, 2 — DISPOFF+SLPIN)
 *  Виводи — зі схеми ES3C35P (docs/ES3C35P) і прикладів LCDwiki.
 *  ------------------------------------------------------------------------- */
#include <Arduino.h>
#include <Wire.h>
#include <SD_MMC.h>
#include "driver/spi_master.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_timer.h"

/* ---------- виводи ---------- */
#define LCD_CS   10
#define LCD_SCK  12
#define LCD_D0   11
#define LCD_D1   13
#define LCD_D2   14
#define LCD_D3    9
#define LCD_BL   41
#define LCD_TE   42
#define I2C_SDA  38
#define I2C_SCL  39
#define TP_RST   48
#define TP_INT   47
#define TP_ADDR  0x55
#define ES_ADDR  0x18
#define I2S_MCK  17
#define I2S_BCK  18
#define I2S_WS   21
#define I2S_DO   15
#define I2S_DI   16
#define PA_EN     1      /* підсилювач SC8002B: низький рівень = увімкнено */
#define LED_PIN  40
#define BAT_PIN   8
#define SD_CLK    5
#define SD_CMD    4
#define SD_D0     6
#define SD_D1     7
#define SD_D2     2
#define SD_D3     3

#define PW 320           /* панель у власній (портретній) системі */
#define PH 480

/* ---------- екран ---------- */
typedef struct { uint8_t cmd; const uint8_t* data; uint8_t len; uint16_t delay_ms; } InitCmd;
/*  Послідовність виробника — з прикладу Example_01 LCDwiki (ydedox/st77922) і
    Example_08 (esp_bsp.c), однакова, крім 0x71/0xBE; беремо esp_bsp.c (LVGL, ним
    плата прошита на заводі).  */
static const InitCmd INIT[] = {
  {0xF1, (const uint8_t*)"\x00", 1, 0},
  {0x60, (const uint8_t*)"\x00\x00\x00", 3, 0},
  {0x65, (const uint8_t*)"\x80", 1, 0},
  {0x79, (const uint8_t*)"\x06", 1, 0},
  {0x7B, (const uint8_t*)"\x00\x08\x08", 3, 0},
  {0x80, (const uint8_t*)"\x55\x62\x2F\x17\xF0\x52\x70\xD2\x52\x62\xEA", 11, 0},
  {0x81, (const uint8_t*)"\x26\x52\x72\x27", 4, 0},
  {0x84, (const uint8_t*)"\x92\x25", 2, 0},
  {0x87, (const uint8_t*)"\x10\x10\x58\x00\x02\x3A", 6, 0},
  {0x88, (const uint8_t*)"\x00\x00\x2C\x10\x04\x00\x00\x00\x01\x01\x01\x01\x01\x00\x06", 15, 0},
  {0x89, (const uint8_t*)"\x00\x00\x00", 3, 0},
  {0x8A, (const uint8_t*)"\x13\x00\x2C\x00\x00\x2C\x10\x10\x00\x3E\x19", 11, 0},
  {0x8B, (const uint8_t*)"\x15\xB1\xB1\x44\x96\x2C\x10\x97\x8E", 9, 0},
  {0x8C, (const uint8_t*)"\x1D\xB1\xB1\x44\x96\x2C\x10\x50\x0F\x01\xC5\x12\x09", 13, 0},
  {0x8D, (const uint8_t*)"\x0C", 1, 0},
  {0x8E, (const uint8_t*)"\x33\x01\x0C\x13\x01\x01", 6, 0},
  {0xB3, (const uint8_t*)"\x00\x30", 2, 0},
  {0xF1, (const uint8_t*)"\x00", 1, 0},
  {0x71, (const uint8_t*)"\xC0", 1, 0},
  {0x66, (const uint8_t*)"\x02\x3F", 2, 0},
  {0xBE, (const uint8_t*)"\x24\x00\x9D", 3, 0},
  {0x70, (const uint8_t*)"\x01\xA0\x11\x40\xE0\x00\x11\x69\x11\x00\x00\x1A", 12, 0},
  {0x90, (const uint8_t*)"\x04\x04\x55\x74\x00\x40\x43\x27\x27", 9, 0},
  {0x91, (const uint8_t*)"\x04\x04\x55\x75\x00\x40\x42\x27\x27", 9, 0},
  {0x92, (const uint8_t*)"\x04\x44\x55\xC0\x06\x00\x07\x05\x90\x27", 10, 0},
  {0x93, (const uint8_t*)"\x04\x43\x11\x00\x00\x00\x00\x05\x90\x27", 10, 0},
  {0x94, (const uint8_t*)"\x00\x00\x00\x00\x00\x00", 6, 0},
  {0x95, (const uint8_t*)"\x96\x16\x00\x00\xFF", 5, 0},
  {0x96, (const uint8_t*)"\x44\x53\x03\x12\x23\x24\x06\x05\x94\x27\x00\x44", 12, 0},
  {0x97, (const uint8_t*)"\x44\x53\x47\x56\x20\x20\x02\x01\x94\x27\x00\x44", 12, 0},
  {0xBA, (const uint8_t*)"\x55\x94\x2D\x94\x27", 5, 0},
  {0x9A, (const uint8_t*)"\x40\x00\x06\x00\x00\x00\x00", 7, 0},
  {0x9B, (const uint8_t*)"\x00\x00\x06\x00\x00\x00\x00", 7, 0},
  {0x9C, (const uint8_t*)"\x5C\x12\x00\x00\x10\x12\x00\x00\x10\x02\x00\x00\x00", 13, 0},
  {0x9D, (const uint8_t*)"\x8A\x51\x00\x00\x00\x80\x1E\x01", 8, 0},
  {0x9E, (const uint8_t*)"\x51\x00\x00\x00\x80\x1E\x01", 7, 0},
  {0xB4, (const uint8_t*)"\x1D\x1C\x1E\x0B\x14\x02\x13\x09\x1E\x00\x1E\x10", 12, 0},
  {0xB5, (const uint8_t*)"\x1D\x1C\x1E\x0A\x15\x03\x11\x08\x1E\x01\x1E\x12", 12, 0},
  {0xB6, (const uint8_t*)"\x77\x77\x00\x0A\xFF\x0A\xFF", 7, 0},
  {0x86, (const uint8_t*)"\xCD\x04\xB1\x02\x58\x12\x58\x0C\x13\x01\xA5\x00\xA5\xA5", 14, 0},
  {0xB7, (const uint8_t*)"\x07\x0A\x0E\x06\x05\x03\x2B\x03\x03\x42\x07\x10\x10\x2E\x3F\x0D", 16, 0},
  {0xB8, (const uint8_t*)"\x07\x0A\x0D\x05\x05\x02\x2B\x02\x03\x42\x06\x10\x0F\x2E\x3F\x0D", 16, 0},
  {0xB9, (const uint8_t*)"\x23\x23", 2, 0},
  {0xBF, (const uint8_t*)"\x10\x14\x14\x0B\x0B\x0B", 6, 0},
  {0xF2, (const uint8_t*)"\x00", 1, 0},
  {0x73, (const uint8_t*)"\x04\xDA\x12\x54\x47", 5, 0},
  {0x77, (const uint8_t*)"\x6B\x5B\xFD\xC3\xC5", 5, 0},
  {0x7A, (const uint8_t*)"\x15\x27", 2, 0},
  {0x7B, (const uint8_t*)"\x04\x57", 2, 0},
  {0x7E, (const uint8_t*)"\x01\x0E", 2, 0},
  {0xBF, (const uint8_t*)"\x36", 1, 0},
  {0xE3, (const uint8_t*)"\x40\x40", 2, 0},
  {0xF0, (const uint8_t*)"\x00", 1, 0},
  {0xD0, (const uint8_t*)"\x00", 1, 0},
  {0x2A, (const uint8_t*)"\x00\x00\x01\x3F", 4, 0},
  {0x2B, (const uint8_t*)"\x00\x00\x01\xDF", 4, 0},
  {0x21, nullptr, 0, 0},
  {0x11, nullptr, 0, 120},
  {0x29, nullptr, 0, 0},
  {0x2C, nullptr, 0, 0},
  {0x3A, (const uint8_t*)"\x01", 1, 0},
  {0x36, (const uint8_t*)"\x00", 1, 0},
  {0x35, (const uint8_t*)"\x01", 1, 20},
};

static spi_device_handle_t s_lcd = nullptr;
static uint32_t s_hz = 80000000;
static uint16_t* s_buf = nullptr;            /* внутрішня пам'ять, DMA */
static const size_t BUF_PX = 4096;

static void lcdBus(uint32_t hz){
  if(s_lcd){ spi_bus_remove_device(s_lcd); s_lcd = nullptr; }
  spi_device_interface_config_t dc = {};
  dc.command_bits = 8;
  dc.address_bits = 24;
  dc.mode = 0;
  dc.clock_speed_hz = (int)hz;
  dc.spics_io_num = LCD_CS;
  dc.flags = SPI_DEVICE_HALFDUPLEX;
  dc.queue_size = 8;
  ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dc, &s_lcd));
  s_hz = hz;
}

static void lcdBegin(){
  spi_bus_config_t bc = {};
  bc.data0_io_num = LCD_D0; bc.data1_io_num = LCD_D1; bc.sclk_io_num = LCD_SCK;
  bc.data2_io_num = LCD_D2; bc.data3_io_num = LCD_D3;
  bc.data4_io_num = -1; bc.data5_io_num = -1; bc.data6_io_num = -1; bc.data7_io_num = -1;
  bc.max_transfer_sz = 32768;
  bc.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD | SPICOMMON_BUSFLAG_IOMUX_PINS;
  esp_err_t e = spi_bus_initialize(SPI2_HOST, &bc, SPI_DMA_CH_AUTO);
  Serial.printf("QSPI: шина %s\n", e == ESP_OK ? "ok" : esp_err_to_name(e));
  lcdBus(s_hz);
  s_buf = (uint16_t*)heap_caps_malloc(BUF_PX * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
}

static void lcdCmd(uint8_t cmd, const uint8_t* data, uint8_t len){
  spi_transaction_t t = {};
  t.cmd = 0x02;
  t.addr = (uint32_t)cmd << 8;
  if(len && len <= 4){ t.flags = SPI_TRANS_USE_TXDATA; memcpy(t.tx_data, data, len); }
  else t.tx_buffer = data;
  t.length = (size_t)len * 8;
  spi_device_polling_transmit(s_lcd, &t);
}

/*  Читання регістра в однопровідному режимі (код 0x0B), dummy — тактів очікування  */
static void lcdRead(uint8_t cmd, uint8_t* out, uint8_t len, uint8_t dummy){
  spi_transaction_ext_t t = {};
  t.base.flags = SPI_TRANS_VARIABLE_DUMMY;
  t.base.cmd = 0x0B;
  t.base.addr = (uint32_t)cmd << 8;
  t.dummy_bits = dummy;
  t.base.length = 0;
  t.base.rxlength = (size_t)len * 8;
  t.base.rx_buffer = out;
  spi_device_polling_transmit(s_lcd, (spi_transaction_t*)&t);
}

static void lcdInit(){
  for(size_t i = 0; i < sizeof(INIT) / sizeof(INIT[0]); i++){
    lcdCmd(INIT[i].cmd, INIT[i].data, INIT[i].len);
    if(INIT[i].delay_ms) delay(INIT[i].delay_ms);
  }
}

static void lcdWin(uint16_t x, uint16_t y, uint16_t w, uint16_t h){
  uint8_t a[4] = { (uint8_t)(x >> 8), (uint8_t)x, (uint8_t)((x + w - 1) >> 8), (uint8_t)(x + w - 1) };
  uint8_t b[4] = { (uint8_t)(y >> 8), (uint8_t)y, (uint8_t)((y + h - 1) >> 8), (uint8_t)(y + h - 1) };
  lcdCmd(0x2A, a, 4);
  lcdCmd(0x2B, b, 4);
}

/*  Пікселі вже в порядку дисплея (старший байт першим)  */
static void lcdPixels(const uint16_t* px, size_t n, bool first){
  spi_transaction_t t = {};
  t.flags = SPI_TRANS_MODE_QIO;
  t.cmd = 0x32;
  t.addr = (first ? 0x2C : 0x3C) << 8;
  t.tx_buffer = px;
  t.length = n * 16;
  spi_device_polling_transmit(s_lcd, &t);
}

static uint16_t sw16(uint16_t c){ return (c >> 8) | (c << 8); }

/*  Прямокутник у системі панелі  */
static void panelFill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t c){
  lcdWin(x, y, w, h);
  size_t n = (size_t)w * h, k = n < BUF_PX ? n : BUF_PX;
  for(size_t i = 0; i < k; i++) s_buf[i] = sw16(c);
  bool first = true;
  while(n){ size_t m = n < BUF_PX ? n : BUF_PX; lcdPixels(s_buf, m, first); first = false; n -= m; }
}

/*  Поворот: 0 — як панель (320×480), 1 — ландшафт (екранний x уздовж панелі y,
    екранний y — проти панелі x), 2 — 180°, 3 — інший ландшафт.  */
static uint8_t s_rot = 1;
static uint16_t scrW(){ return (s_rot & 1) ? PH : PW; }
static uint16_t scrH(){ return (s_rot & 1) ? PW : PH; }

/*  Заливка в екранній системі: перетворюємо прямокутник у прямокутник панелі  */
static void scrFill(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c){
  switch(s_rot){
    case 0: panelFill(x, y, w, h, c); break;
    case 1: panelFill(PW - y - h, x, h, w, c); break;
    case 2: panelFill(PW - x - w, PH - y - h, w, h, c); break;
    default: panelFill(y, PH - x - w, h, w, c); break;
  }
}

/*  Тестова картина в екранній системі: тло, рамка, кольорові кути (червоний —
    верх-ліво, зелений — верх-право, синій — низ-ліво, білий — низ-право),
    смуги через 40 пікселів уздовж верхнього краю.  */
static void pattern(uint8_t r){
  s_rot = r;
  const uint16_t W = scrW(), H = scrH();
  uint32_t t0 = micros();
  scrFill(0, 0, W, H, 0x2104);
  scrFill(0, 0, W, 4, 0xFFE0); scrFill(0, H - 4, W, 4, 0xFFE0);
  scrFill(0, 0, 4, H, 0xFFE0); scrFill(W - 4, 0, 4, H, 0xFFE0);
  scrFill(8, 8, 48, 48, 0xF800);
  scrFill(W - 56, 8, 48, 48, 0x07E0);
  scrFill(8, H - 56, 48, 48, 0x001F);
  scrFill(W - 56, H - 56, 48, 48, 0xFFFF);
  for(int x = 64; x < W - 64; x += 40) scrFill(x, 12, 20, 20, 0xFD20);
  /*  стрілка «вгору» в центрі: стовпчик і трикутник зі смуг  */
  scrFill(W / 2 - 4, H / 2 - 20, 8, 70, 0xFFFF);
  for(int i = 0; i < 24; i += 4) scrFill(W / 2 - i - 2, H / 2 - 44 + i, 2 * i + 4, 4, 0xFFFF);
  Serial.printf("картина: поворот %u (%ux%u), %lu мкс\n", r, W, H, (unsigned long)(micros() - t0));
}

/* ---------- I2C ---------- */
static void i2cScan(){
  Serial.print("I2C 38/39:");
  int n = 0;
  for(uint8_t a = 1; a < 127; a++){
    Wire.beginTransmission(a);
    if(Wire.endTransmission() == 0){ Serial.printf(" 0x%02X", a); n++; }
  }
  Serial.printf("  (%d)\n", n);
}

static bool tpRd(uint16_t reg, uint8_t* d, uint8_t n){
  Wire.beginTransmission(TP_ADDR);
  Wire.write((uint8_t)(reg >> 8)); Wire.write((uint8_t)reg);
  if(Wire.endTransmission(false) != 0) return false;
  if(Wire.requestFrom((uint8_t)TP_ADDR, n) != n) return false;
  for(uint8_t i = 0; i < n; i++) d[i] = Wire.read();
  return true;
}

static void tpReset(){
  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW); delay(100);
  digitalWrite(TP_RST, HIGH); delay(100);
  pinMode(TP_INT, INPUT_PULLUP);
}

static void tpInfo(){
  uint8_t v = 0, rev[4] = {0}, inf[5] = {0};
  bool a = tpRd(0x0000, &v, 1), b = tpRd(0x000C, rev, 4), c = tpRd(0x0005, inf, 5);
  Serial.printf("сенсор: %s версія %u (%u.%u.%u.%u), maxX %u maxY %u, точок %u, INT=%d\n",
                (a && b && c) ? "ok" : "НЕ ВІДПОВІДАЄ", v, rev[0], rev[1], rev[2], rev[3],
                (inf[0] << 8) | inf[1], (inf[2] << 8) | inf[3], inf[4], digitalRead(TP_INT));
}

/*  Один опит: кількість дотиків і перший у системі панелі  */
static int tpPoll(uint16_t& x, uint16_t& y){
  uint8_t info = 0;
  if(!tpRd(0x0010, &info, 1)) return -1;
  if(!(info & 0x08)) return 0;
  uint8_t mx = 0;
  if(!tpRd(0x0009, &mx, 1)) return -1;
  if(mx > 5) mx = 5;
  uint8_t rep[35];
  if(!mx || !tpRd(0x0014, rep, mx * 7)) return 0;
  int k = 0;
  for(int i = 0; i < mx; i++){
    uint8_t* p = rep + i * 7;
    if(!(p[0] & 0x80)) continue;
    if(!k){ x = ((p[0] & 0x3F) << 8) | p[1]; y = (p[2] << 8) | p[3]; }
    k++;
  }
  return k;
}

static void tpLog(uint32_t secs){
  Serial.printf("дотики %lu с (система панелі 320x480; INT на старті %d)\n", (unsigned long)secs, digitalRead(TP_INT));
  uint32_t end = millis() + secs * 1000, polls = 0, hits = 0, errs = 0;
  int was = 0;
  while((int32_t)(millis() - end) < 0){
    uint16_t x = 0, y = 0;
    int k = tpPoll(x, y);
    polls++;
    if(k < 0) errs++;
    if(k > 0){
      hits++;
      if(!was || hits % 10 == 0) Serial.printf("  дотик %d: x=%u y=%u INT=%d\n", k, x, y, digitalRead(TP_INT));
    }
    if(was > 0 && k == 0) Serial.println("  відпущено");
    was = k;
    delay(10);
  }
  Serial.printf("опитів %lu, з дотиком %lu, помилок %lu\n", (unsigned long)polls, (unsigned long)hits, (unsigned long)errs);
}

/* ---------- кодек ES8311 + I2S ---------- */
static bool esW(uint8_t r, uint8_t v){ Wire.beginTransmission(ES_ADDR); Wire.write(r); Wire.write(v); return Wire.endTransmission() == 0; }
static bool esR(uint8_t r, uint8_t& v){
  Wire.beginTransmission(ES_ADDR); Wire.write(r);
  if(Wire.endTransmission(false) != 0) return false;
  if(Wire.requestFrom((uint8_t)ES_ADDR, (uint8_t)1) != 1) return false;
  v = Wire.read(); return true;
}
/*  Те саме, що es8311_begin() радіо (MCLK = 256·fs, 16 біт, I2S), плюс мікрофон  */
static bool esBegin(uint32_t rate){
  uint8_t id = 0, v = 0;
  if(!esR(0xFD, id)) { Serial.println("ES8311 не відповідає"); return false; }
  esW(0x00, 0x1F); delay(20); esW(0x00, 0x00); esW(0x00, 0x80);
  esW(0x01, 0x3F);
  if(esR(0x06, v)) esW(0x06, v & ~0x20);
  /*  делітелі для 256·fs  */
  if(esR(0x02, v)) esW(0x02, (v & 0x07));
  esW(0x03, 0x10); esW(0x04, 0x10); esW(0x05, 0x00);
  if(esR(0x06, v)) esW(0x06, (v & 0xE0) | 0x03);
  if(esR(0x07, v)) esW(0x07, v & 0xC0);
  esW(0x08, 0xFF);
  if(esR(0x00, v)) esW(0x00, v & 0xBF);
  esW(0x09, 0x0C); esW(0x0A, 0x0C);
  esW(0x0D, 0x01); esW(0x0E, 0x02); esW(0x12, 0x00); esW(0x13, 0x10);
  esW(0x1C, 0x6A); esW(0x37, 0x08);
  esW(0x32, 0xBF);                       /* гучність ЦАПа = 0 дБ (як ES8311_VOLUME 75) */
  if(esR(0x31, v)) esW(0x31, v & ~0x60);
  /*  мікрофон: як es8311_mic(5) радіо, лівий слот — мікрофон, правий — ЦАП  */
  esW(0x17, 0xC8); esW(0x14, 0x1A); esW(0x16, 5); esW(0x44, 0x58);
  Serial.printf("ES8311 id=0x%02X, %u Гц\n", id, (unsigned)rate);
  return true;
}

static i2s_chan_handle_t s_tx = nullptr, s_rx = nullptr;
static bool i2sBegin(uint32_t rate){
  if(s_tx) return true;
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.auto_clear = true;
  if(i2s_new_channel(&cc, &s_tx, &s_rx) != ESP_OK) return false;
  i2s_std_config_t sc = {};
  sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
  sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  sc.gpio_cfg.mclk = (gpio_num_t)I2S_MCK; sc.gpio_cfg.bclk = (gpio_num_t)I2S_BCK; sc.gpio_cfg.ws = (gpio_num_t)I2S_WS;
  sc.gpio_cfg.dout = (gpio_num_t)I2S_DO; sc.gpio_cfg.din = (gpio_num_t)I2S_DI;
  if(i2s_channel_init_std_mode(s_tx, &sc) != ESP_OK) return false;
  if(i2s_channel_init_std_mode(s_rx, &sc) != ESP_OK) return false;
  i2s_channel_enable(s_tx); i2s_channel_enable(s_rx);
  return true;
}

/*  Гьорцель: потужність частоти f у блоці  */
static float goertzel(const int16_t* x, int n, int stride, float f, float fs){
  float w = 2 * PI * f / fs, c = 2 * cosf(w), s1 = 0, s2 = 0;
  for(int i = 0; i < n; i++){ float s = x[i * stride] + c * s1 - s2; s2 = s1; s1 = s; }
  float p = s1 * s1 + s2 * s2 - c * s1 * s2;
  return 10 * log10f(p / ((float)n * n / 4) / (32768.f * 32768.f) + 1e-20f);
}

/*  Тон 1 кГц −12 дБ повної шкали і запис мікрофона: до, під час, після.
    Так звук перевіряється без вуха: мікрофон плати чує динамік.  */
static void tone(float secs){
  const uint32_t FS = 16000;
  if(!i2sBegin(FS) || !esBegin(FS)){ Serial.println("звук: не вдалось запустити"); return; }
  pinMode(PA_EN, OUTPUT); digitalWrite(PA_EN, LOW);   /* підсилювач увімкнено */
  delay(400);                                          /* підсилювач прокидається */
  static int16_t tx[512 * 2], rx[512 * 2];
  size_t bw, br;
  float ph = 0, dph = 2 * PI * 1000 / FS;
  /*  фази: 0 — тиша 0,5 с, 1 — тон, 2 — тиша 0,5 с  */
  float sum[3] = {0}, sumR[3] = {0}; int cnt[3] = {0};
  uint32_t n0 = FS / 2, n1 = (uint32_t)(FS * secs), n2 = FS / 2, done = 0;
  while(done < n0 + n1 + n2){
    int ph_i = done < n0 ? 0 : done < n0 + n1 ? 1 : 2;
    for(int i = 0; i < 512; i++){
      int16_t s = ph_i == 1 ? (int16_t)(8192 * sinf(ph)) : 0;
      ph += dph; if(ph > 2 * PI) ph -= 2 * PI;
      tx[2 * i] = s; tx[2 * i + 1] = s;
    }
    i2s_channel_write(s_tx, tx, sizeof(tx), &bw, 200);
    i2s_channel_read(s_rx, rx, sizeof(rx), &br, 200);
    if(done > 1024){                                /* перші блоки — перехідні */
      sum[ph_i] += powf(10, goertzel(rx, 512, 2, 1000, FS) / 10);
      sumR[ph_i] += powf(10, goertzel(rx + 1, 512, 2, 1000, FS) / 10);
      cnt[ph_i]++;
    }
    done += 512;
  }
  digitalWrite(PA_EN, HIGH);
  for(int i = 0; i < 3; i++){
    float m = cnt[i] ? 10 * log10f(sum[i] / cnt[i] + 1e-20f) : -999;
    float r = cnt[i] ? 10 * log10f(sumR[i] / cnt[i] + 1e-20f) : -999;
    Serial.printf("  %s: мікрофон 1 кГц %.1f дБ, правий слот (ЦАП) %.1f дБ, блоків %d\n",
                  i == 0 ? "тиша до" : i == 1 ? "тон    " : "тиша піс", m, r, cnt[i]);
  }
}

/* ---------- решта ---------- */
static void sdTest(){
  SD_MMC.end();
  if(!SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3)){ Serial.println("SD: setPins не вдалось"); return; }
  bool ok = SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_DEFAULT, 5);
  if(!ok){ Serial.println("SD: картка не змонтувалась (немає картки?)"); return; }
  Serial.printf("SD: тип %d, розмір %llu МБ, зайнято %llu МБ\n", (int)SD_MMC.cardType(),
                SD_MMC.cardSize() / (1024 * 1024), SD_MMC.usedBytes() / (1024 * 1024));
  File r = SD_MMC.open("/"); int n = 0;
  for(File f = r.openNextFile(); f && n < 12; f = r.openNextFile(), n++) Serial.printf("  %s%s %u\n", f.name(), f.isDirectory() ? "/" : "", (unsigned)f.size());
}

static void bat(){
  uint32_t a = 0; for(int i = 0; i < 16; i++) a += analogReadMilliVolts(BAT_PIN);
  Serial.printf("батарея: %u мВ на виводі → %u мВ (дільник 1:2)\n", (unsigned)(a / 16), (unsigned)(a / 8));
}

static void teCount(){
  pinMode(LCD_TE, INPUT);
  uint32_t t0 = millis(), edges = 0; int last = digitalRead(LCD_TE);
  while(millis() - t0 < 1000){ int v = digitalRead(LCD_TE); if(v != last){ edges++; last = v; } }
  Serial.printf("TE: %lu перемикань за 1 с (≈ %lu кадрів/с)\n", (unsigned long)edges, (unsigned long)(edges / 2));
}

static void heap(){
  Serial.printf("пам'ять: внутрішня вільна %u (найбільший шмат %u), PSRAM вільна %u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void info(){
  Serial.printf("ES3C35P пробна: чип %s rev %d, %u МГц, флеш %u МБ, PSRAM %u МБ\n", ESP.getChipModel(), ESP.getChipRevision(),
                (unsigned)ESP.getCpuFreqMHz(), (unsigned)(ESP.getFlashChipSize() >> 20), (unsigned)(ESP.getPsramSize() >> 20));
  heap();
}

static void readId(){
  for(uint8_t d = 0; d <= 16; d += 8){
    uint8_t id[4] = {0}, pm[2] = {0}, mad[2] = {0}, col[2] = {0};
    lcdRead(0x04, id, 3, d); lcdRead(0x0A, pm, 1, d); lcdRead(0x0B, mad, 1, d); lcdRead(0x0C, col, 1, d);
    Serial.printf("  dummy %2u: ID %02X %02X %02X, RDDPM %02X, MADCTL %02X, COLMOD %02X\n", d, id[0], id[1], id[2], pm[0], mad[0], col[0]);
  }
}

/*  Читання відеопам'яті: вікно w×1 у системі панелі, RAMRD (0x2E)  */
static void rdRam(uint16_t x, uint16_t y, uint16_t w, uint8_t dummy){
  if(w > 32) w = 32;
  lcdWin(x, y, w, 1);
  uint8_t b[100] = {0};
  lcdRead(0x2E, b, w * 3, dummy);
  Serial.printf("RAMRD (%u,%u) w=%u dummy=%u:", x, y, w, dummy);
  for(int i = 0; i < w * 3; i++) Serial.printf(" %02X", b[i]);
  Serial.println();
}


/* ---------- переривання сенсора і сон ---------- */
static volatile uint32_t s_intFall = 0, s_intLowMin = 0xFFFFFFFF, s_intLowMax = 0, s_intT0 = 0;
static void IRAM_ATTR intIsr(){
  uint32_t t = micros();
  if(!digitalRead(TP_INT)){ s_intFall++; s_intT0 = t; }
  else if(s_intT0){ uint32_t d = t - s_intT0; if(d < s_intLowMin) s_intLowMin = d; if(d > s_intLowMax) s_intLowMax = d; s_intT0 = 0; }
}
/*  Імпульси INT: скільки спадів і яка тривалість низького рівня — під час дотиків  */
static void intWidth(uint32_t secs){
  s_intFall = 0; s_intLowMin = 0xFFFFFFFF; s_intLowMax = 0; s_intT0 = 0;
  pinMode(TP_INT, INPUT_PULLUP);
  attachInterrupt(TP_INT, intIsr, CHANGE);
  Serial.printf("INT: слухаю %lu с — торкайтесь екрана\n", (unsigned long)secs);
  uint32_t end = millis() + secs * 1000, touchPolls = 0;
  while((int32_t)(millis() - end) < 0){ uint16_t x, y; if(tpPoll(x, y) > 0) touchPolls++; delay(20); }
  detachInterrupt(TP_INT);
  Serial.printf("INT: спадів %lu, низький рівень від %lu до %lu мкс; опитів із дотиком %lu\n", (unsigned long)s_intFall,
                (unsigned long)(s_intLowMin == 0xFFFFFFFF ? 0 : s_intLowMin), (unsigned long)s_intLowMax, (unsigned long)touchPolls);
}

/*  Чи бачить сенсор дотик, коли панель погашена: mode 1 — лише DISPOFF,
    2 — DISPOFF + SLPIN (сон панелі). Підсвітка вимкнена.  */
static void sleepTest(uint8_t mode, uint32_t secs){
  analogWrite(LCD_BL, 0);
  lcdCmd(0x28, nullptr, 0); delay(20);
  if(mode == 2){ lcdCmd(0x10, nullptr, 0); delay(120); }
  s_intFall = 0; s_intT0 = 0; s_intLowMin = 0xFFFFFFFF; s_intLowMax = 0;
  pinMode(TP_INT, INPUT_PULLUP);
  attachInterrupt(TP_INT, intIsr, CHANGE);
  Serial.printf("СОН ПАНЕЛІ (режим %u: %s): %lu с — торкайтесь екрана\n", mode, mode == 2 ? "DISPOFF+SLPIN" : "DISPOFF", (unsigned long)secs);
  uint32_t end = millis() + secs * 1000, hits = 0, errs = 0;
  while((int32_t)(millis() - end) < 0){ uint16_t x, y; int k = tpPoll(x, y); if(k > 0) hits++; if(k < 0) errs++; delay(20); }
  detachInterrupt(TP_INT);
  if(mode == 2){ lcdCmd(0x11, nullptr, 0); delay(120); }
  lcdCmd(0x29, nullptr, 0); delay(20);
  analogWrite(LCD_BL, 200);
  pattern(s_rot);
  Serial.printf("СОН ПАНЕЛІ: спадів INT %lu (низький %lu..%lu мкс), опитів із дотиком %lu, помилок I2C %lu\n", (unsigned long)s_intFall,
                (unsigned long)(s_intLowMin == 0xFFFFFFFF ? 0 : s_intLowMin), (unsigned long)s_intLowMax, (unsigned long)hits, (unsigned long)errs);
}

/*  Справжній легкий сон до дотику (пробудження GPIO47 низьким рівнем), панель —
    як у sleepTest. Межа — secs (таймер), щоб плата не заснула назавжди.  */
static void lightSleep(uint8_t mode, uint32_t secs){
  analogWrite(LCD_BL, 0);
  lcdCmd(0x28, nullptr, 0); delay(20);
  if(mode == 2){ lcdCmd(0x10, nullptr, 0); delay(120); }
  pinMode(TP_INT, INPUT_PULLUP);
  Serial.printf("ЛЕГКИЙ СОН (панель: %s) до дотику, не довше %lu с — торкніться екрана\n", mode == 2 ? "SLPIN" : "DISPOFF", (unsigned long)secs);
  Serial.flush(); delay(50);
  gpio_wakeup_enable((gpio_num_t)TP_INT, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
  int64_t t0 = esp_timer_get_time();
  esp_light_sleep_start();
  int64_t dt = esp_timer_get_time() - t0;
  esp_sleep_wakeup_cause_t c = esp_sleep_get_wakeup_cause();
  gpio_wakeup_disable((gpio_num_t)TP_INT);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  uint16_t x = 0, y = 0; int k = tpPoll(x, y);
  if(mode == 2){ lcdCmd(0x11, nullptr, 0); delay(120); }
  lcdCmd(0x29, nullptr, 0); delay(20);
  analogWrite(LCD_BL, 200);
  pattern(s_rot);
  Serial.printf("ЛЕГКИЙ СОН: прокинулась через %lld мс, причина %s, дотик зараз %d (%u,%u)\n", (long long)(dt / 1000),
                c == ESP_SLEEP_WAKEUP_GPIO ? "GPIO (сенсор)" : c == ESP_SLEEP_WAKEUP_TIMER ? "таймер" : "інша", k, x, y);
}

static String s_line;
static void cmd(String l){
  l.trim();
  int sp = l.indexOf(' ');
  String c = sp < 0 ? l : l.substring(0, sp), a = sp < 0 ? "" : l.substring(sp + 1);
  if(c == "info") info();
  else if(c == "heap") heap();
  else if(c == "i2c") i2cScan();
  else if(c == "lcd"){ lcdInit(); Serial.println("екран: послідовність виробника надіслано"); }
  else if(c == "pat") pattern(a.length() ? a.toInt() : 1);
  else if(c == "rid") readId();
  else if(c == "rd"){ int x = 0, y = 0, w = 4, d = 8; sscanf(a.c_str(), "%d %d %d %d", &x, &y, &w, &d); rdRam(x, y, w, d); }
  else if(c == "fillp"){ int x, y, w, h; unsigned col; if(sscanf(a.c_str(), "%d %d %d %d %x", &x, &y, &w, &h, &col) == 5){ panelFill(x, y, w, h, col); Serial.println("ok"); } }
  else if(c == "madctl"){ uint8_t v = strtoul(a.c_str(), nullptr, 16); lcdCmd(0x36, &v, 1); Serial.printf("MADCTL=%02X\n", v); }
  else if(c == "spi"){ lcdBus((uint32_t)a.toInt() * 1000000UL); Serial.printf("QSPI %u Гц\n", (unsigned)s_hz); }
  else if(c == "bl"){ analogWrite(LCD_BL, a.toInt()); Serial.printf("підсвітка %d\n", (int)a.toInt()); }
  else if(c == "tp") tpInfo();
  else if(c == "tlog") tpLog(a.length() ? a.toInt() : 10);
  else if(c == "tone") tone(a.length() ? a.toFloat() : 1.5f);
  else if(c == "sd") sdTest();
  else if(c == "bat") bat();
  else if(c == "led"){ int r = 0, g = 0, b = 0; sscanf(a.c_str(), "%d %d %d", &r, &g, &b); rgbLedWrite(LED_PIN, r, g, b); Serial.printf("світлодіод %d %d %d\n", r, g, b); }
  else if(c == "te") teCount();
  else if(c == "intw") intWidth(a.length() ? a.toInt() : 10);
  else if(c == "slp"){ int m = 2, t = 15; sscanf(a.c_str(), "%d %d", &m, &t); sleepTest(m, t); }
  else if(c == "lsleep"){ int m = 2, t = 30; sscanf(a.c_str(), "%d %d", &m, &t); lightSleep(m, t); }
  else if(c.length()) Serial.printf("невідома команда: %s\n", c.c_str());
}

void setup(){
  Serial.begin(115200);
  pinMode(PA_EN, OUTPUT); digitalWrite(PA_EN, HIGH);    /* підсилювач вимкнено, поки не треба */
  pinMode(LCD_BL, OUTPUT); digitalWrite(LCD_BL, LOW);
  delay(300);
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  tpReset();
  lcdBegin();
  lcdInit();
  pattern(1);
  analogWrite(LCD_BL, 200);
  rgbLedWrite(LED_PIN, 0, 0, 16);
  Serial.println("##PROBE# готово");
}

void loop(){
  while(Serial.available()){
    char ch = Serial.read();
    if(ch == '\n' || ch == '\r'){ if(s_line.length()) cmd(s_line); s_line = ""; }
    else if(s_line.length() < 120) s_line += ch;
  }
  delay(2);
}
