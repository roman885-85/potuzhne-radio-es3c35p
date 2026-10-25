#include "../core/options.h"
#if I2S_ES8311

#include "yoES8311.h"
#include <Wire.h>

/*  Регистры ES8311, названия — как в даташите  */
#define ES8311_RESET_REG00        0x00
#define ES8311_CLK_MANAGER_REG01  0x01
#define ES8311_CLK_MANAGER_REG02  0x02
#define ES8311_CLK_MANAGER_REG03  0x03
#define ES8311_CLK_MANAGER_REG04  0x04
#define ES8311_CLK_MANAGER_REG05  0x05
#define ES8311_CLK_MANAGER_REG06  0x06
#define ES8311_CLK_MANAGER_REG07  0x07
#define ES8311_CLK_MANAGER_REG08  0x08
#define ES8311_SDPIN_REG09        0x09
#define ES8311_SDPOUT_REG0A       0x0A
#define ES8311_SYSTEM_REG0D       0x0D
#define ES8311_SYSTEM_REG0E       0x0E
#define ES8311_SYSTEM_REG12       0x12
#define ES8311_SYSTEM_REG13       0x13
#define ES8311_SYSTEM_REG14       0x14
#define ES8311_ADC_REG1C          0x1C
#define ES8311_DAC_REG31          0x31
#define ES8311_DAC_REG32          0x32
#define ES8311_DAC_REG37          0x37
#define ES8311_CHD1_REGFD         0xFD    /* идентификатор чипа, должен быть 0x83 */

#ifndef ES8311_MCLK_MULTIPLE
  #define ES8311_MCLK_MULTIPLE  256       /* столько же ставит драйвер I2S */
#endif

static bool     _ready = false;
static uint32_t _rate  = 0;

/* --------------------------------------------------------------------------- */
static bool wr(uint8_t reg, uint8_t val) {
  Wire.beginTransmission((uint8_t)ES8311_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool rd(uint8_t reg, uint8_t &val) {
  Wire.beginTransmission((uint8_t)ES8311_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)ES8311_ADDR, (uint8_t)1) != 1) return false;
  val = Wire.read();
  return true;
}

/*  Делители кодека. MCLK всегда кратен частоте дискретизации, поэтому вместо
 *  громоздкой таблицы из примера достаточно разобрать саму кратность.          */
static bool clock_dividers(uint32_t mclk, uint32_t rate, uint8_t &pre_div, uint8_t &pre_multi) {
  if (rate == 0) return false;
  switch (mclk / rate) {
    case 128: pre_div = 1; pre_multi = 1; break;   /* x2 */
    case 256: pre_div = 1; pre_multi = 0; break;   /* x1 */
    case 384: pre_div = 3; pre_multi = 1; break;   /* x2 / 3 */
    case 512: pre_div = 2; pre_multi = 0; break;
    case 768: pre_div = 3; pre_multi = 0; break;
    default:  return false;
  }
  return (mclk % rate) == 0;
}

bool es8311_set_sample_rate(uint32_t rate) {
  if (!_ready || rate == 0) return false;
  uint8_t pre_div, pre_multi, regv;
  const uint32_t mclk = rate * ES8311_MCLK_MULTIPLE;

  if (!clock_dividers(mclk, rate, pre_div, pre_multi)) {
    Serial.printf("##[ERROR]#\tES8311: частота %u Гц не поддержана\n", rate);
    return false;
  }

  if (!rd(ES8311_CLK_MANAGER_REG02, regv)) return false;
  regv &= 0x07;
  regv |= (uint8_t)((pre_div - 1) << 5);
  regv |= (uint8_t)(pre_multi << 3);
  if (!wr(ES8311_CLK_MANAGER_REG02, regv)) return false;

  wr(ES8311_CLK_MANAGER_REG03, 0x10);       /* fs_mode = 0, adc_osr = 0x10 */
  wr(ES8311_CLK_MANAGER_REG04, 0x10);       /* dac_osr = 0x10              */
  wr(ES8311_CLK_MANAGER_REG05, 0x00);       /* adc_div = dac_div = 1       */

  if (!rd(ES8311_CLK_MANAGER_REG06, regv)) return false;
  regv &= 0xE0;
  regv |= 0x03;                             /* bclk_div = 4                */
  wr(ES8311_CLK_MANAGER_REG06, regv);

  if (!rd(ES8311_CLK_MANAGER_REG07, regv)) return false;
  regv &= 0xC0;                             /* lrck_h = 0                  */
  wr(ES8311_CLK_MANAGER_REG07, regv);
  wr(ES8311_CLK_MANAGER_REG08, 0xFF);       /* lrck_l                      */

  _rate = rate;
  return true;
}

bool es8311_set_volume(uint8_t vol) {
  if (!_ready) return false;
  if (vol > 100) vol = 100;
  return wr(ES8311_DAC_REG32, vol == 0 ? 0 : (uint8_t)((vol * 256 / 100) - 1));
}

bool es8311_mute(bool mute) {
  if (!_ready) return false;
  uint8_t reg31;
  if (!rd(ES8311_DAC_REG31, reg31)) return false;
  if (mute) reg31 |=  (uint8_t)(0x60);
  else      reg31 &= (uint8_t)~(0x60);
  return wr(ES8311_DAC_REG31, reg31);
}

bool es8311_ready() { return _ready; }
int  es8311_id(){ uint8_t v = 0; return rd(ES8311_CHD1_REGFD, v) ? (int)v : -1; }
uint32_t es8311_rate(){ return _rate; }

/*  Читаємо назад те, що самі записали: якщо кодек живий і налаштований,
    регістри збігатимуться з очікуваними.  */
bool es8311_write(uint8_t reg, uint8_t val){ return wr(reg, val); }

void es8311_dump(){
  if(!_ready){ Serial.println("ES8311: не ініціалізований"); return; }
  static const uint8_t regs[] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0D,0x0E,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1A,0x1B,0x1C,0x31,0x32,0x44,0x45,0xFD};
  Serial.printf("ES8311: частота=%u Гц, гучність ЦАПа=%d\n", _rate, (int)ES8311_VOLUME);
  for(uint8_t i=0;i<sizeof(regs);i++){
    uint8_t v=0;
    Serial.printf("  reg%02X=%s ", regs[i], rd(regs[i], v) ? "" : "??");
    Serial.printf("0x%02X%s", v, (i%6==5)?"\n":"");
  }
  Serial.println();
}

bool es8311_begin(uint32_t sample_rate) {
  Wire.begin(I2C_SDA, I2C_SCL, 400000);

  uint8_t id = 0;
  if (!rd(ES8311_CHD1_REGFD, id)) {
    Serial.println("##[ERROR]#\tES8311 не отвечает по I2C");
    return false;
  }

  /*  Сброс и включение питания  */
  wr(ES8311_RESET_REG00, 0x1F);
  delay(20);
  wr(ES8311_RESET_REG00, 0x00);
  wr(ES8311_RESET_REG00, 0x80);

  /*  Тактирование: все клоки включены, MCLK берётся с вывода MCLK  */
  wr(ES8311_CLK_MANAGER_REG01, 0x3F);
  uint8_t reg06 = 0;
  if (rd(ES8311_CLK_MANAGER_REG06, reg06)) {
    reg06 &= (uint8_t)~0x20;                /* SCLK не инвертирован */
    wr(ES8311_CLK_MANAGER_REG06, reg06);
  }

  _ready = true;
  if (!es8311_set_sample_rate(sample_rate)) { _ready = false; return false; }

  /*  Формат: кодек — ведомый, I2S, 16 бит  */
  uint8_t reg00 = 0;
  if (rd(ES8311_RESET_REG00, reg00)) wr(ES8311_RESET_REG00, reg00 & 0xBF);
  wr(ES8311_SDPIN_REG09,  0x0C);            /* 16 бит на входе  */
  wr(ES8311_SDPOUT_REG0A, 0x0C);            /* 16 бит на выходе */

  wr(ES8311_SYSTEM_REG0D, 0x01);            /* аналоговая часть под напряжением */
  wr(ES8311_SYSTEM_REG0E, 0x02);
  wr(ES8311_SYSTEM_REG12, 0x00);            /* ЦАП включён                      */
  wr(ES8311_SYSTEM_REG13, 0x10);            /* выход на усилитель наушников     */
  wr(ES8311_ADC_REG1C,    0x6A);
  wr(ES8311_DAC_REG37,    0x08);            /* эквалайзер ЦАПа в обход          */

  es8311_set_volume(ES8311_VOLUME);
  es8311_mute(false);

  Serial.printf("ES8311 id=0x%02X, %u Гц, громкость %d\n", id, sample_rate, (int)ES8311_VOLUME);
  return true;
}

/*  Крючок, который зовёт Audio::setSampleRate() сразу после того, как драйвер
 *  I2S переставил MCLK. Без него при переходе, скажем, с 44100 на 48000 кодек
 *  остался бы со старыми делителями и звук поехал бы по высоте.                */
void audio_samplerate_changed(uint32_t rate) {
  if (!_ready || rate == 0 || rate == _rate) return;
  es8311_set_sample_rate(rate);
}

#endif  // I2S_ES8311

/*  Той самий порядок, що в es8311_suspend() з ESP-ADF: гучність у нуль,
    АЦП/ЦАП і аналогові джерела знеструмити. Прокидається кодек лише
    повним es8311_begin() — після сну радіо однаково стартує наново.  */
/*  Вбудований мікрофон: аналоговий вхід, PGA на максимум, цифрове підсилення
    АЦП кроками по 6 дБ (0..7 = 0..42 дБ), гучність АЦП — як у прикладі
    виробника (0xC8). Регістри ті самі, що в es8311_microphone_config().  */
bool es8311_mic(uint8_t gain_step) {
  if (gain_step > 7) gain_step = 7;
  bool ok = true;
  ok &= wr(0x17, 0xC8);                  /* гучність АЦП */
  ok &= wr(ES8311_SYSTEM_REG14, 0x1A);   /* аналоговий мікрофон, PGA максимум */
  ok &= wr(0x16, gain_step);             /* підсилення АЦП */
  /*  Лівий слот АЦП — мікрофон, правий — те, що грає ЦАП, зсередини кодека:
      опорний сигнал для віднімання луни, вирівняний до відліку. Так робить
      сам Espressif (esp_codec_dev, es8311.c: «internal reference signal
      (ADCL + DACR)»).  */
  ok &= wr(0x44, 0x58);
  return ok;
}

bool es8311_suspend() {
  bool ok = true;
  ok &= wr(ES8311_DAC_REG32, 0x00);
  ok &= wr(0x17, 0x00);                  /* гучність АЦП */
  ok &= wr(ES8311_SYSTEM_REG0E, 0xFF);
  ok &= wr(ES8311_SYSTEM_REG12, 0x02);
  ok &= wr(ES8311_SYSTEM_REG14, 0x00);
  ok &= wr(ES8311_SYSTEM_REG0D, 0xFA);
  ok &= wr(0x15, 0x00);
  ok &= wr(ES8311_DAC_REG37, 0x08);
  ok &= wr(0x45, 0x01);
  return ok;
}
