#include "yoSfx.h"
#include <LittleFS.h>
#include <driver/i2s.h>
#include "esp_heap_caps.h"
#include "../core/options.h"
#include "../core/player.h"
#include "yoExtras.h"

YoSfx sfx;

const uint16_t YoSfx::DEFAULT_MASK = (1 << SFX_START) | (1 << SFX_GESTURE) | (1 << SFX_CONNECT) |
                                     (1 << SFX_ERROR) | (1 << SFX_TIMER) | (1 << SFX_ALARM) | (1 << SFX_LOWBAT);

static const char* const IDS[SFX_N]    = { "start", "click", "gesture", "connect", "error", "timer", "alarm", "battery" };
static const char* const TITLES[SFX_N] = { "Увімкнення", "Дотик до екрана", "Жест прийнято", "Мережа з'явилась",
                                           "Мережа зникла", "Таймер сну", "Будильник", "Батарея сідає" };

const char* YoSfx::id(SfxEvent e)    { return e < SFX_N ? IDS[e] : ""; }
const char* YoSfx::title(SfxEvent e) { return e < SFX_N ? TITLES[e] : ""; }
int YoSfx::find(const char* s){
  if(!s || !*s) return -1;
  if(s[0] >= '0' && s[0] <= '9'){ int n = atoi(s); return n >= 0 && n < SFX_N ? n : -1; }
  for(int i = 0; i < SFX_N; i++) if(!strcmp(s, IDS[i])) return i;
  return -1;
}

enum : uint8_t { Q_TEST = 0x80, Q_RELOAD = 0x40 };

static bool s_mounted = false, s_mountTried = false;
bool YoSfx::mounted(){ return s_mounted; }
bool YoSfx::mount(){
  if(s_mountTried) return s_mounted;
  s_mountTried = true;
  /*  Розділ «assets» є лише після прошивки з новою таблицею розділів. Немає
      його (оновлювали через сторінку) — просто без звуків і заставки.  */
  s_mounted = LittleFS.begin(false, "/assets", 6, "assets");
  if(!s_mounted) s_mounted = LittleFS.begin(true, "/assets", 6, "assets");
  return s_mounted;
}

void YoSfx::begin(){
  if(_q) return;
  _fsOk = mount();
  if(_fsOk){ LittleFS.mkdir("/snd"); LittleFS.mkdir("/snd/user"); refreshUser(); }
  Serial.printf("##[BOOT]#\tзвуки подій: %s\n", _fsOk ? "розділ ресурсів є" : "розділу ресурсів немає");
  _lock = xSemaphoreCreateMutex();
  _q = xQueueCreate(6, sizeof(uint8_t));
  /*  вище за задачу з'єднання станції (3): інакше рукостискання TLS рвало звук  */
  xTaskCreatePinnedToCore(_taskFn, "sfx", 4096, this, 3, nullptr, 1);   /* нижче за головний цикл: він веде звук */
}

void YoSfx::refreshUser(){
  uint16_t m = 0;
  for(uint8_t i = 0; i < SFX_N; i++) if(userFile((SfxEvent)i)) m |= 1U << i;
  _userMask = m;
}

size_t YoSfx::fsTotal(){ return _fsOk ? LittleFS.totalBytes() : 0; }
size_t YoSfx::fsUsed(){ return _fsOk ? LittleFS.usedBytes() : 0; }

bool YoSfx::userFile(SfxEvent e){
  if(!_fsOk || e >= SFX_N) return false;
  char p[40]; snprintf(p, sizeof(p), "/snd/user/%s.wav", IDS[e]);
  return LittleFS.exists(p);
}

/*  Звук увімкнення — частина заставки: його вмикає й гучність задає «звук заставки»
    (розробник), а не вимикач звуків подій.  */
static bool enabledFor(SfxEvent e){
  const ExtStore& s = extras.s;
  if(e == SFX_START) return s.splashVol > 0;
  /*  Батарея — свій повзунок (див. _gainQ15), тож і вмикається ним: загальна
      гучність на нулі не має ховати попередження.  */
  if(e == SFX_LOWBAT) return s.sfxOn && s.sfxEvVol[e] && (s.sfxMask & (1U << e));
  return s.sfxOn && s.sfxVol && (s.sfxMask & (1U << e));
}

bool YoSfx::willPlay(SfxEvent e) const {
  return _q && _fsOk && e < SFX_N && enabledFor(e);
}

void YoSfx::play(SfxEvent e){
  if(!_q || e >= SFX_N) return;
  if(!enabledFor(e)) return;
  uint8_t m = e;
  xQueueSend(_q, &m, 0);                  /* черга повна — цей звук пропускаємо */
}

void YoSfx::test(SfxEvent e){
  if(!_q || e >= SFX_N) return;
  uint8_t m = e | Q_TEST;
  xQueueSend(_q, &m, 0);
}

void YoSfx::reload(SfxEvent e){
  if(!_q || e >= SFX_N) return;
  uint8_t m = e | Q_RELOAD;
  xQueueSend(_q, &m, pdMS_TO_TICKS(50));
}

uint32_t YoSfx::clipMs(SfxEvent e){
  const Clip* c = _get(e);
  return c && c->rate ? (uint32_t)((uint64_t)c->len * 1000 / c->rate) : 0;
}

/*  Гучність звуків — своя, від 0 до 100, квадратом: на слух рівніше.  */
int32_t YoSfx::_gainQ15(SfxEvent e){
  /*  Привітання (заставка) — своя гучність; решта — загальна гучність звуків,
      помножена на гучність саме цієї події.  */
  uint32_t v = e == SFX_START ? extras.s.splashVol : extras.s.sfxVol;
  if(v > 100) v = 100;
  if(e != SFX_START && e < sizeof(extras.s.sfxEvVol)){
    uint32_t ev = extras.s.sfxEvVol[e]; if(ev > 100) ev = 100;
    /*  Попередження про батарею має бути чути й тоді, коли решта звуків тиха,
        тому воно слухає лише свій повзунок. Раніше замість цього стояв нижній
        поріг у 40 %: усе нижче нього не мало жодного впливу, і звук був завжди
        гучний. Тепер повзунок працює від 0 до 100.  */
    v = e == SFX_LOWBAT ? ev : v * ev / 100;
  }
  return (int32_t)(32767UL * v * v / 10000UL);
}

const YoSfx::Clip* YoSfx::_get(SfxEvent e){
  if(e >= SFX_N) return nullptr;
  if(xSemaphoreTake(_lock, pdMS_TO_TICKS(500)) != pdTRUE) return nullptr;
  if(!_clip[e].tried) _load(e);
  const Clip* c = _clip[e].pcm ? &_clip[e] : nullptr;
  xSemaphoreGive(_lock);
  return c;
}

static uint32_t rd32(const uint8_t* b){ return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }
static uint16_t rd16(const uint8_t* b){ return b[0] | (b[1] << 8); }

/*  WAV: PCM 16 біт, моно чи стерео (стерео зводимо в моно), 8–48 кГц, до 10 с.  */
/*  Двічі низхідний сигнал (880 → 660 Гц): розділу ресурсів на старих радіо може
    не бути, а попередження мусить звучати й там.  */
bool YoSfx::_synthLowBat(Clip& c){
  const uint32_t rate = 22050;
  const uint32_t n = rate * 62 / 100;
  int16_t* pcm = (int16_t*)heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if(!pcm) return false;
  struct Tone { float t0, len, hz; };
  static const Tone T[2] = { { 0.00f, 0.16f, 880.0f }, { 0.26f, 0.30f, 660.0f } };
  for(uint32_t i = 0; i < n; i++){
    const float t = (float)i / rate;
    float v = 0;
    for(const Tone& k : T){
      const float u = t - k.t0;
      if(u < 0 || u > k.len) continue;
      float env = 1.0f;
      if(u < 0.008f) env = u / 0.008f;
      else if(u > k.len - 0.04f) env = (k.len - u) / 0.04f;
      v += env * (sinf(2 * (float)M_PI * k.hz * u) + 0.25f * sinf(4 * (float)M_PI * k.hz * u));
    }
    pcm[i] = (int16_t)(v * 0.5f * 32767);
  }
  c.pcm = pcm; c.len = n; c.rate = rate;
  return true;
}

bool YoSfx::_load(SfxEvent e){
  Clip& c = _clip[e];
  c.tried = true;
  if(!_fsOk) return e == SFX_LOWBAT && _synthLowBat(c);
  char path[40];
  snprintf(path, sizeof(path), "/snd/user/%s.wav", IDS[e]);
  File f = LittleFS.open(path, "r");
  if(!f){ snprintf(path, sizeof(path), "/snd/default/%s.wav", IDS[e]); f = LittleFS.open(path, "r"); }
  if(!f) return e == SFX_LOWBAT && _synthLowBat(c);
  uint8_t h[12];
  if(f.read(h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)){ f.close(); return false; }
  uint16_t fmt = 0, ch = 0, bits = 0; uint32_t rate = 0, dataLen = 0;
  bool data = false;
  while(f.available() >= 8){
    uint8_t ck[8];
    if(f.read(ck, 8) != 8) break;
    uint32_t sz = rd32(ck + 4);
    if(!memcmp(ck, "fmt ", 4)){
      uint8_t fb[16];
      if(sz < 16 || f.read(fb, 16) != 16) break;
      fmt = rd16(fb); ch = rd16(fb + 2); rate = rd32(fb + 4); bits = rd16(fb + 14);
      if(sz > 16) f.seek(f.position() + (sz - 16) + (sz & 1));
    }else if(!memcmp(ck, "data", 4)){
      dataLen = sz; data = true; break;
    }else{
      f.seek(f.position() + sz + (sz & 1));
    }
  }
  if(!data || fmt != 1 || bits != 16 || (ch != 1 && ch != 2) || rate < 8000 || rate > 48000){
    Serial.printf("##SFX#\t%s: не той формат (формат %u, %u біт, %u кан., %u Гц)\n", path, fmt, bits, ch, (unsigned)rate);
    f.close(); return false;
  }
  uint32_t frames = dataLen / (2U * ch);
  if(frames > rate * 10) frames = rate * 10;              /* свій звук — до 10 с */
  if(frames < 2){ f.close(); return false; }
  int16_t* pcm = (int16_t*)heap_caps_malloc(frames * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if(!pcm){ f.close(); return false; }
  uint32_t got = 0;
  if(ch == 1){
    got = f.read((uint8_t*)pcm, frames * 2) / 2;
  }else{
    int16_t tmp[256];
    while(got < frames){
      uint32_t want = frames - got; if(want > 128) want = 128;
      size_t r = f.read((uint8_t*)tmp, want * 4) / 4;
      if(!r) break;
      for(size_t i = 0; i < r; i++) pcm[got + i] = (int16_t)(((int32_t)tmp[2*i] + tmp[2*i+1]) / 2);
      got += r;
    }
  }
  f.close();
  if(got < 2){ heap_caps_free(pcm); return false; }
  c.pcm = pcm; c.len = got; c.rate = rate;
  return true;
}

void YoSfx::_taskFn(void* p){ ((YoSfx*)p)->_run(); }

void YoSfx::_run(){
  for(;;){
    uint8_t m;
    if(xQueueReceive(_q, &m, portMAX_DELAY) != pdTRUE) continue;
    SfxEvent e = (SfxEvent)(m & 0x3F);
    if(e >= SFX_N) continue;
    if(m & Q_RELOAD){
      /*  не звільняти те, що саме домішується  */
      while(_mixPcm && _mixPcm == _clip[e].pcm) vTaskDelay(pdMS_TO_TICKS(20));
      if(xSemaphoreTake(_lock, pdMS_TO_TICKS(1000)) == pdTRUE){
        if(_clip[e].pcm) heap_caps_free(_clip[e].pcm);
        _clip[e] = Clip{};
        xSemaphoreGive(_lock);
      }
      continue;
    }
    const Clip* c = _get(e);
    if(!c) continue;
    int32_t g = _gainQ15(e);
    if(!g) continue;
    if(player.isRunning()){
      /*  станція грає — домішує задача звуку  */
      _mixPcm = nullptr;
      _mixPos = 0; _mixRate = c->rate; _stepFor = 0; _mixGain = g; _mixLen = c->len;
      _mixPcm = c->pcm;
      _audible[e] = millis() | 1;
      /*  чекаємо кінця: наступний звук у черзі — після цього  */
      uint32_t t0 = millis(), dur = (uint32_t)((uint64_t)c->len * 1000 / c->rate) + 300;
      while(_mixPcm && millis() - t0 < dur){
        vTaskDelay(pdMS_TO_TICKS(20));
        if(_mixPcm && !player.isRunning()){
          /*  станцію зупинили посеред звуку (хлопки «пауза») — доіграємо самі  */
          uint64_t at = _mixPos;
          _mixPcm = nullptr;
          if((at >> 16) + 1 < c->len) _out(*c, g, at, e);
          break;
        }
      }
      _mixPcm = nullptr;
    }else{
      _out(*c, g, 0, e);
    }
  }
}

/*  Радіо мовчить: самі пишемо в I2S. Підсилювач після вимкнення прокидається
    ~0,4 с — спершу тиша, інакше початок звуку губиться (як у перевірці жестів).  */
void YoSfx::_out(const Clip& c, int32_t g, uint64_t startPos, SfxEvent ev){
  _outBusy = true;
  /*  Вбудований підсилювач чіпаємо, лише коли звук іде саме на нього. Раніше
      знімали приглушення завжди — і звук події (зокрема привітання заставки)
      лунав із вбудованого динаміка навіть тоді, коли вибрано зовнішній ЦАП.  */
  const bool ownAmp = extras.s.dac == 0 && MUTE_PIN != 255;
  const bool extAmp = extras.s.dac != 0 && extras.s.extMute;           /* MUTE зовнішнього підсилювача під'єднано */
  bool wasMuted = (ownAmp && digitalRead(MUTE_PIN) == MUTE_VAL) || (extAmp && !extras.extAmpOn());
  if(ownAmp) digitalWrite(MUTE_PIN, !MUTE_VAL);
  if(extAmp) extras.extAmp(true);
  uint32_t rate = player.getSampleRate();
  if(rate < 8000) rate = 44100;
  const size_t FR = 256;
  int16_t buf[FR * 2];
  size_t w = 0;
  uint32_t lead = wasMuted ? rate * 35 / 100 : 0;
  memset(buf, 0, sizeof(buf));
  while(lead && !player.isRunning()){
    uint32_t n = lead > FR ? FR : lead;
    i2s_write(I2S_NUM_0, buf, n * 4, &w, pdMS_TO_TICKS(500));
    lead -= n;
  }
  uint32_t step = (uint32_t)(((uint64_t)c.rate << 16) / rate);
  uint64_t pos = startPos;
  bool handed = false;
  if(ev < SFX_N && !startPos){
    _audible[ev] = millis() | 1;                     /* підсилювач прокинувся — звук іде зараз */
    if(ev == SFX_START) Serial.printf("##SFX#\tзвук увімкнення пішов на %u мс\n", (unsigned)millis());
  }
  for(;;){
    if(player.isRunning()){
      /*  посеред звуку заграла станція — решту домішає задача звуку  */
      _mixPcm = nullptr;
      _mixPos = pos; _mixRate = c.rate; _stepFor = 0; _mixGain = g; _mixLen = c.len;
      _mixPcm = c.pcm;
      handed = true;
      break;
    }
    size_t k = 0;
    for(; k < FR; k++){
      uint32_t i = (uint32_t)(pos >> 16);
      if(i + 1 >= c.len) break;
      int32_t a = c.pcm[i], b = c.pcm[i + 1];
      int32_t v = a + (int32_t)(((int64_t)(b - a) * (int32_t)(pos & 0xFFFF)) >> 16);
      int32_t fx = (v * g) >> 15;
      buf[2*k] = buf[2*k + 1] = (int16_t)fx;
      pos += step;
    }
    if(k) i2s_write(I2S_NUM_0, buf, k * 4, &w, pdMS_TO_TICKS(500));
    if(k < FR) break;
  }
  if(!handed){
    /*  хвіст тиші, щоб буфери I2S доіграли, і підсилювач — назад, якщо тиша  */
    memset(buf, 0, sizeof(buf));
    uint32_t tail = rate * 15 / 100;
    while(tail && !player.isRunning()){
      uint32_t n = tail > FR ? FR : tail;
      i2s_write(I2S_NUM_0, buf, n * 4, &w, pdMS_TO_TICKS(500));
      tail -= n;
    }
    if(!player.isRunning() && ownAmp) digitalWrite(MUTE_PIN, MUTE_VAL);
    if(!player.isRunning() && extAmp) extras.extAmp(false);
  }
  _lastEndMs = millis();
  _outBusy = false;
}

/*  Задача звуку, на кожен відлік станції.  */
uint32_t YoSfx::mix(uint32_t s32, uint32_t rate){
  const int16_t* p = _mixPcm;
  uint32_t n = _mixLen;
  if(!p || n < 2) return s32;
  if(rate < 8000) rate = 44100;
  if(rate != _stepFor){ _stepFor = rate; _mixStep = (uint32_t)(((uint64_t)_mixRate << 16) / rate); }
  uint32_t i = (uint32_t)(_mixPos >> 16);
  if(i + 1 >= n){ _mixPcm = nullptr; _lastEndMs = millis(); return s32; }
  int32_t a = p[i], b = p[i + 1];
  int32_t v = a + (int32_t)(((int64_t)(b - a) * (int32_t)(_mixPos & 0xFFFF)) >> 16);
  _mixPos += _mixStep;
  int32_t fx = (v * _mixGain) >> 15;
  /*  музика на час звуку тихіше на 6 дБ, з плавним входом і виходом по 10 мс  */
  uint32_t ramp = _mixRate / 100, left = n - i;
  uint32_t k = i < ramp ? i : (left < ramp ? left : ramp);
  int32_t duck = 32767 - (int32_t)((16384UL * k) / (ramp ? ramp : 1));
  int32_t l = (int16_t)(s32 >> 16), r = (int16_t)(s32 & 0xFFFF);
  l = ((l * duck) >> 15) + fx;
  r = ((r * duck) >> 15) + fx;
  if(l > 32767) l = 32767; else if(l < -32768) l = -32768;
  if(r > 32767) r = 32767; else if(r < -32768) r = -32768;
  return ((uint32_t)(uint16_t)l << 16) | (uint16_t)r;
}
