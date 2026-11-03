#include "yoMic.h"
#include "yoATrace.h"
#include <driver/i2s.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "esp_vad.h"
#include "esp_aec.h"
#include "dsps_fft2r.h"
#include "dsps_wind.h"
#include "../core/options.h"
#include "../core/player.h"
#include "yoI2sLock.h"
#include "../ES8311/yoES8311.h"
#include "yoExtras.h"
#include "yoDsp.h"
#include "yoSfx.h"
#include "../core/network.h"
#include "../core/config.h"
#include "../menu/yoMenu.h"          /* він і вмикає USE_YOMENU */

YoMic mic;

static const int  MIC_FS = 16000;   /* усе, що аналізуємо, — 16 кГц моно */
static const int  BLK    = 512;     /* 32 мс: блок для ударів і кадр esp_aec */
static const int  VADN   = 480;     /* 30 мс: кадр WebRTC VAD */

bool YoMic::listening() const {
  /*  Мікрофон працює при будь-якому виході. Раніше його глушив зовнішній ЦАП —
      вважалось, що кодек лишається без тактів. Насправді ні: перепризначення
      виводів не знімає старі з матриці GPIO, тож такт і слово далі приходять
      на кодек, а головний такт (MCLK) і лінія мікрофона (DIN) при переході на
      зовнішній ЦАП узагалі не чіпаються. Те саме доводить і давня вада, яку
      довелось лікувати окремо: вбудований динамік продовжував грати разом із
      зовнішнім ЦАПом — а це можливо лише коли такти до кодека доходять.  */
  return running() && (extras.s.micOn || atrace::on);   /* atrace: мікрофон чує, що звучить у кімнаті */
}

MicGesture YoMic::takeGesture(){
  uint8_t g = _gesture; _gesture = MG_NONE;
  return (MicGesture)g;
}

void YoMic::apply(){
  /*  Підсилення мікрофона кодек приймає шиною I2C — до виводів I2S це
      відношення не має, тож виставляємо його при будь-якому виході.  */
  uint8_t g = extras.s.micGain ? extras.s.micGain - 1 : 4;   /* типове — 24 дБ */
  es8311_mic(g);
}

void YoMic::gainTemp(uint8_t step){ es8311_mic(step > 7 ? 7 : step); }

void YoMic::begin(){
  if(_task) return;
  apply();
  /*  Пріоритет 0 — як у задачі простою: коли віднімання луни забирає все ядро,
      FreeRTOS ділить час між ними по черзі, і сторож задач не спрацьовує
      (дамп 1.4.5: сторож, у задачі mic). Решта роботи ядра 0 її й так випереджає.  */
  xTaskCreatePinnedToCore(_taskFn, "mic", 8192, this, 3, &_task, 0);   /* не 0: на дні його витісняли, і слух губив хлопки */
}

void YoMic::_taskFn(void* p){ ((YoMic*)p)->_run(); }

/*  ---------- стан аналізу (одна задача — статичні змінні досить) ---------- */
static float*   fftBuf = nullptr;      /* 2*BLK: re, im */
static float*   fftRef = nullptr;      /* те саме для опорного (що грає радіо) */
static float*   win = nullptr;
static float*   mag1 = nullptr, *ph1 = nullptr, *ph2 = nullptr;   /* у PSRAM */
static float    onMu = 0, onDev = 0;
static int8_t onLvl[4] = {0};         /* рівні цих ударів — для журналу */
/*  Що було перед ударом: мова (по кадру WebRTC VAD на 30 мс, 1 — мова) і
    гучність блоків (≈ 0,5 с) разом із фоном того часу.  */
static uint32_t vadRing = 0;
static int8_t   lvlRing[16] = {0}, nzRing[16] = {0};
static uint32_t btRing[16] = {0};
static uint8_t  lvlPos = 0;
static uint32_t onT[4] = {0};          /* часи останніх ударів одного виду */
static uint8_t  onKind[4] = {0};       /* 1 хлопок, 2 стук */
static uint8_t  onN = 0;
static vad_handle_t vad = nullptr;
static int16_t  vadBuf[VADN];
static uint16_t vadFill = 0;
static uint8_t  vadRun = 0;            /* кадрів голосу поспіль */
static aec_handle_t* aec = nullptr;
static int16_t* aecOut = nullptr;

/*  Читаємо I2S, зводимо до 16 кГц, складаємо блоки по 512.  */
void YoMic::_run(){
  const size_t RAW = 1024;                                  /* стерео 16 біт: 256 кадрів */
  /*  Внутрішньої пам'яті в радіо обмаль (її їдять Wi-Fi, TLS, екран) — усе
      робоче мікрофона кладемо в PSRAM: 31 блок на секунду там устигає.  */
  const uint32_t MEMPS = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  int16_t* raw = (int16_t*)heap_caps_malloc(RAW, MEMPS);
  int16_t* blk = (int16_t*)heap_caps_aligned_alloc(16, BLK * sizeof(int16_t), MEMPS);
  int16_t* rblk = (int16_t*)heap_caps_aligned_alloc(16, BLK * sizeof(int16_t), MEMPS);
  fftBuf = (float*)heap_caps_aligned_alloc(16, BLK * 2 * sizeof(float), MEMPS);
  fftRef = (float*)heap_caps_aligned_alloc(16, BLK * 2 * sizeof(float), MEMPS);
  win    = (float*)heap_caps_malloc(BLK * sizeof(float), MEMPS);
  aecOut = (int16_t*)heap_caps_aligned_alloc(16, BLK * sizeof(int16_t), MEMPS);
  mag1 = (float*)heap_caps_calloc(BLK/2, sizeof(float), MEMPS);
  ph1  = (float*)heap_caps_calloc(BLK/2, sizeof(float), MEMPS);
  ph2  = (float*)heap_caps_calloc(BLK/2, sizeof(float), MEMPS);
  if(!raw || !blk || !rblk || !fftBuf || !fftRef || !win || !aecOut || !mag1 || !ph1 || !ph2){ _task = nullptr; vTaskDelete(NULL); return; }
  dsps_fft2r_init_fc32(NULL, BLK);
  dsps_wind_hann_f32(win, BLK);
  vad = vad_create_with_param(VAD_MODE_3, MIC_FS, 30, 200, 600);

  uint16_t n = 0;
  uint32_t ph = 0;
  int32_t dc = 0, rdc = 0, acc = 0, racc = 0;
  uint16_t cnt = 0;
  uint32_t firRate = 0, reads = 0;
  int16_t yPrev = 0, rPrev = 0;
  for(;;){
    if(_swReq){ _swReq = false; _sweep(raw); n = 0; continue; }
    if(!listening()){ vTaskDelay(pdMS_TO_TICKS(200)); n = 0; continue; }
    if(_tsKind) _simChunk(raw);                               /* перевірка: хлопки чи стук через свій динамік */
    size_t got = 0;
    esp_err_t rr = ESP_FAIL;
    {
      YoI2sGuard lk(pdMS_TO_TICKS(100));
      if(lk.ok) rr = i2s_read(I2S_NUM_0, raw, RAW, &got, pdMS_TO_TICKS(60));
    }
    if(rr != ESP_OK || got < 4){
      /*  драйвер саме перенастроюють (зміна частоти, зупинка) — не крутимось
          упусту: ядро 0 без простою будить сторожовий таймер  */
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    /*  Хоч раз на ~90 мс віддаємо процесор. Із відніманням луни задача важка, і
        коли екран саме гортає довгий список, вона не встигала: дані I2S завжди
        чекали, i2s_read більше не блокувався, задача простою ядра 0 не
        отримувала часу 5 с — і сторож перезавантажував радіо. Краще втратити
        шматочок звуку з мікрофона.  */
    if((reads & 15) == 15) vTaskDelay(1);
    if(atrace::on){ int32_t pk = 0; for(size_t i = 0; i < got / 4; i++){ const int32_t v = abs((int32_t)raw[2*i]); if(v > pk) pk = v; } atrace::mic(pk); }
    uint32_t rate = player.getSampleRate();
    if(rate < 8000) rate = 44100;
    if(rate != firRate){ _firDesign(rate); firRate = rate; ph = 0; }
    size_t fr = got / 4;
    if((++reads & 7) == 0 && fr){                             /* рівні слотів — для відладки, зрідка */
      int64_t eL = 0, eR = 0;
      for(size_t i = 0; i < fr; i++){ eL += (int32_t)raw[2*i]*raw[2*i]; eR += (int32_t)raw[2*i+1]*raw[2*i+1]; }
      float rl = sqrt((float)eL/fr), rr = sqrt((float)eR/fr);
      _slotL = rl > 1 ? (int8_t)lroundf(20.0f*log10f(rl/32768.0f)) : -90;
      _slotR = rr > 1 ? (int8_t)lroundf(20.0f*log10f(rr/32768.0f)) : -90;
    }
    /*  Луну віднімаємо — тоді до 16 кГц зводимо точно й однаково обидва слоти:
        фільтр нижніх частот (КІХ, зріз 7 кГц) і відлік у потрібний момент
        лінійною інтерполяцією. Проста «середня з 2–3 відліків» міняє крок від
        відліку до відліку, і лінійний фільтр луни не повторить такий шлях.
        Без віднімання луни вистачає й середньої — вона вп'ятеро дешевша.  */
    bool exact = _aecWant;
    for(size_t i = 0; i < fr; i++){
      int32_t v = raw[2*i];
      dc += (v * 256 - dc) >> 10;   v -= dc >> 8;            /* постійна складова */
      if(v > 32767) v = 32767; else if(v < -32768) v = -32768;
      if(exact){
        int32_t r = raw[2*i+1];
        rdc += (r * 256 - rdc) >> 10;  r -= rdc >> 8;
        if(r > 32767) r = 32767; else if(r < -32768) r = -32768;
        int16_t y = _fir(0, (int16_t)v), q = _fir(1, (int16_t)r);
        ph += MIC_FS;
        if(ph >= rate){
          ph -= rate;
          int32_t f = (int32_t)((ph << 12) / MIC_FS);        /* частка назад від цього входу, 0..4095 */
          blk[n]  = (int16_t)(y + (((int32_t)yPrev - y) * f >> 12));
          rblk[n] = (int16_t)(q + (((int32_t)rPrev - q) * f >> 12));
          if(++n == BLK){ _feed(blk, rblk); n = 0; }
        }
        yPrev = y; rPrev = q;
      }else{
        acc += v; racc += raw[2*i+1]; cnt++;
        ph += MIC_FS;
        if(ph >= rate){
          ph -= rate;
          blk[n] = (int16_t)(acc / (cnt ? cnt : 1));
          rblk[n] = (int16_t)(racc / (cnt ? cnt : 1));
          acc = 0; racc = 0; cnt = 0;
          if(++n == BLK){ _feed(blk, rblk); n = 0; }
        }
      }
    }
  }
}

/*  КІХ нижніх частот для зведення до 16 кГц: віконний sinc (Блекмен), зріз
    7 кГц на частоті потоку, коефіцієнти в Q15.  */
static const int FIR_N = 23;                /* непарне: симетричний, половина множень */
static int16_t firC[FIR_N];
static int16_t firH[2][2 * FIR_N];         /* подвоєна історія — без переносу через край */
static uint8_t firI[2] = {0, 0};

void YoMic::_firDesign(uint32_t rate){
  double fc = 7000.0 / rate, sum = 0, c[FIR_N];
  for(int k = 0; k < FIR_N; k++){
    int m = k - (FIR_N - 1) / 2;
    double h = m == 0 ? 2.0 * fc : sin(2.0 * M_PI * fc * m) / (M_PI * m);
    double w = 0.42 - 0.5 * cos(2.0 * M_PI * k / (FIR_N - 1)) + 0.08 * cos(4.0 * M_PI * k / (FIR_N - 1));
    c[k] = h * w; sum += c[k];
  }
  for(int k = 0; k < FIR_N; k++) firC[k] = (int16_t)lround(c[k] / sum * 32767.0);
  memset(firH, 0, sizeof(firH));
}

int16_t YoMic::_fir(uint8_t ch, int16_t x){
  uint8_t i = firI[ch];
  int16_t* h = firH[ch];
  h[i] = x; h[i + FIR_N] = x;               /* h[i..i+FIR_N-1] — останні FIR_N відліків підряд */
  firI[ch] = (i + 1) % FIR_N;
  const int16_t* w = h + i + 1;             /* найстаріший … найновіший */
  const int M = (FIR_N - 1) / 2;
  int32_t acc = (int32_t)firC[M] * w[M];
  for(int k = 0; k < M; k++) acc += (int32_t)firC[k] * ((int32_t)w[k] + w[FIR_N - 1 - k]);
  acc >>= 15;
  return (int16_t)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
}

/*  Блок від мікрофона: якщо грає звук і треба чути кімнату — віднімаємо луну
    власного динаміка (esp_aec за опорним сигналом із кодека).  */
void YoMic::_feed(int16_t* x, int16_t* ref){
  if(_exKind) _extMix(x);
  bool playing = player.isRunning();
  bool want = extras.s.micPlay;
  _aecOn = playing && want;
  _aecWant = _aecOn;                             /* наступні блоки — точним зведенням */
  if(_aecOn){
    if(aec && (_aecMode != _aecModeOn || _aecLen != _aecLenOn)){ aec_destroy(aec); aec = nullptr; }
    if(!aec){
      aec = aec_create(MIC_FS, _aecLen, 1, (aec_mode_t)_aecMode);
      _aecModeOn = _aecMode; _aecLenOn = _aecLen;
      if(aec) _aecChunk = aec_get_chunksize(aec);
      _erle = 0;
    }
    if(aec){
      aec_process(aec, x, ref, aecOut);
      double ei = 0, eo = 0;
      for(int i = 0; i < BLK; i++){ ei += (double)x[i] * x[i]; eo += (double)aecOut[i] * aecOut[i]; }
      if(ei > 1e5){
        float cut = (float)(10.0 * log10((ei + 1) / (eo + 1)));
        _erle += (cut - _erle) * 0.1f;
      }
      _block(aecOut, ref, true);
      return;
    }
  }
  _block(x, ref, playing);
}

static float blockDb(const int16_t* x){
  int64_t e = 0;
  for(int i = 0; i < BLK; i++) e += (int32_t)x[i] * x[i];
  float rms = sqrt((float)e / BLK);
  float db = rms > 1 ? 20.0f * log10f(rms / 32768.0f) : -90.0f;
  return db < -90 ? -90 : db;
}

void YoMic::_block(int16_t* x, int16_t* ref, bool playing){
  uint32_t now = millis();
  _blocks++;
  float mdb = blockDb(x);
  int db = (int)lroundf(mdb);
  _prevLvl = _lvl;
  _lvl = (int8_t)db;
  lvlRing[lvlPos] = (int8_t)db; nzRing[lvlPos] = _noise;
  btRing[lvlPos] = (uint32_t)((uint64_t)_blocks * BLK * 1000 / MIC_FS); lvlPos = (lvlPos + 1) & 15;
  if(_dbg && (_tsKind || _exKind)) Serial.printf("##LVL#\t%u %d поз=%u\n", (unsigned)now, db, (unsigned)_tsPos);
  /*  фон: униз одразу, вгору ледь-ледь — кроки й голос його не піднімають  */
  if(db < _noise) _noise = (int8_t)db;
  else if((_blocks & 15) == 0 && _noise < -20) _noise++;
  /*  тло — середній рівень за ~2 с (потужність), без самих ударів  */
  {
    /*  Раніше тло оновлювалось лише тоді, коли звук тихіший за тло + 10 дБ, —
        і музика, що завжди гучніша, його так і не зрушувала (-68 дБ при
        музиці на -28). Тепер тло йде й за стійкою гучністю, а короткий удар
        може підняти його не більше ніж на 10 дБ за блок.  */
    /*  А ще раніше (музика) — піднімалось від самих хлопків: два-три хлопки —
        і тло +10 дБ, наступні вже «тихо». Тепер два темпи: близьке до тла
        (±6 дБ) — звичайно, гучніше — ледь-ледь: музика, що звучить секундами,
        тло доганяє, а хлопок за 30 мс його майже не рухає.  */
    float pw = pow(10.0f, mdb / 10.0f), bg = pow(10.0f, _bgDb / 10.0f);
    if(mdb <= _bgDb + 6.0f) bg += (pw - bg) * 0.03f;
    else { if(pw > bg * 10.0f) pw = bg * 10.0f; bg += (pw - bg) * 0.004f; }
    _bgDb = bg > 1e-10f ? 10.0f * log10(bg) : -100.0f;
  }

  /*  Що з почутого — не наш власний звук. Радіо знає, що грає (правий слот —
      вихід ЦАП), а пара «динамік — мікрофон» у корпусі тримає співвідношення
      рівнів сталим. Вчимо його, поки в кімнаті тихо; перевищення над ним —
      звук кімнати: голос, кроки, хлопок. Так уміє будь-який детектор
      «двобічної розмови» в гучному зв'язку.  */
  float rdb = playing ? blockDb(ref) : -90.0f;
  if(!playing){ _cNLo = 0; _cNHi = 0; }                 /* звук зупинили — наступного разу вчимось наново */
  _refDb = rdb;
  if(rdb > -60.0f){
    float r = mdb - rdb;
    if(_coupleN < 20){ _couple += (r - _couple) / (++_coupleN); }
    else if(r < _couple + 4.0f){ _couple += (r - _couple) * 0.02f; }   /* подвійну розмову не вчимо */
    _excess = mdb - (rdb + _couple);
  }else{
    _excess = mdb - _noise;                       /* своє мовчить — рахуємо від фону */
  }
  bool own = rdb > -60.0f;
  /*  Звук кімнати — стійкий: ~100 мс поспіль (3 блоки) і помітно гучніший —
      над власним звуком на 10 дБ, у тиші на 15 дБ над фоном. Одиночний
      сплеск музики чи далекий стук дверей тишу не перебиває, інакше «довго
      тихо» не настає ніколи.  */
  bool loudNow = own ? _excess > 10.0f : db > _noise + 15;
  _roomRun = loudNow ? (_roomRun < 255 ? _roomRun + 1 : 255) : 0;
  if(_roomRun >= 3){
    _lastSound = now; _lastRoom = now;
    if(_dbg && _roomRun == 3) Serial.printf("##ROOM#\tзвук кімнати: рівень %d, фон %d, понад своїм %.1f дБ\n", db, (int)_noise, _excess);
  }

  /*  голос: WebRTC VAD на кадрах по 30 мс; поки грає — лише те, що
      вибивається над власним звуком (інакше співак у пісні — теж «голос»)  */
  for(int i = 0; i < BLK; i++){
    vadBuf[vadFill++] = x[i];
    if(vadFill == VADN){
      vadFill = 0;
      bool sp = vad && vad_process(vad, vadBuf, MIC_FS, 30) == VAD_SPEECH && db > _noise + 6 && (!own || _excess > 6.0f);
      vadRing = (vadRing << 1) | (sp ? 1U : 0U);
      vadRun = sp ? (vadRun < 255 ? vadRun + 1 : 255) : 0;
      _speech = vadRun >= 4;                       /* ~120 мс поспіль — не клацання, а голос */
      if(_speech){
        if(_dbg && now - _lastVoice > 1000) Serial.printf("##ROOM#\tголос: рівень %d, фон %d\n", db, (int)_noise);
        _lastVoice = now; _lastRoom = now;
      }
    }
  }
  /*  Час ударів — за номером блоку, тобто за моментом запису звуку, а не
      обробки: задача мікрофона ділить ядро з Wi-Fi, і millis() між блоками
      гуляє на ±30 мс, а відлуння (190 мс) від межі «зарано» (220 мс)
      відділяють лічені десятки.  */
  uint32_t bt = (uint32_t)((uint64_t)_blocks * BLK * 1000 / MIC_FS);
  /*  Звук щойно ввімкнули чи вимкнули — підсилювач прокидається (~0,4 с) чи
      клацає, а співвідношення «динамік — мікрофон» ще не те: удари цих
      миттєвостей не рахуємо. Інакше хлопки, що ввімкнули радіо, ловили
      його ж перший такт як новий удар.  */
  if(playing != _wasPlaying){ _wasPlaying = playing; _edgeBt = bt + (playing ? 1200 : 400); }
  /*  …і поки звучить (та ще 0,4 с) власний звук події радіо: чотири ноти
      звуку ввімкнення інакше складались у «2 хлопки».  */
  _quietEdge = bt < _edgeBt || sfx.recent();
  _onset(x, own ? ref : nullptr, bt);
  _pattern(bt);
}

/*  Удари: «complex domain» — наскільки спектр цього блоку не схожий на
    передбачений із двох попередніх (амплітуда й фаза). Метод Duxbury і
    ін., DAFx-03; так рахує aubio і pector на ESP32.  */
void YoMic::_onset(int16_t* x, int16_t* ref, uint32_t now){
  bool clapOn = extras.s.clapOn, knockOn = extras.s.knockOn;
  if(!clapOn && !knockOn){ _onLast = 0; return; }
  for(int i = 0; i < BLK; i++){ fftBuf[2*i] = (x[i] / 32768.0f) * win[i]; fftBuf[2*i+1] = 0; }
  dsps_fft2r_fc32(fftBuf, BLK);
  dsps_bit_rev_fc32(fftBuf, BLK);
  float val = 0, lo = 0, hi = 0;
  double cenN = 0, cenD = 0, tot = 0;
  for(int b = 1; b < BLK/2; b++){
    float re = fftBuf[2*b], im = fftBuf[2*b+1];
    float m = sqrt(re*re + im*im);
    float p = atan2(im, re);
    float d = mag1[b]*mag1[b] + m*m - 2.0f * mag1[b] * m * cos(2.0f*ph1[b] - ph2[b] - p);
    float c = sqrt(fabs(d));
    val += c;
    /*  вид удару — за енергією самого блоку, а не за зміною: початок будь-
        якого удару широкосмуговий, а от тіло в стуку — низьке, у хлопку — високе  */
    if(b >= 2 && b <= 48)   lo += m * m;         /* ~60..1500 Гц: стук по корпусу */
    if(b >= 64 && b <= 224) hi += m * m;         /* ~2..7 кГц: хлопок */
    if(b >= 2 && b <= 224){ cenN += (double)b * m * m; cenD += (double)m * m; }
    tot += (double)m * m;                        /* весь спектр від 31 Гц — щоб бачити гул нижче смуги удару */
    mag1[b] = m; ph2[b] = ph1[b]; ph1[b] = p;
  }
  _onLast = val;
  /*  Гучний неприйнятий удар стає «чужим поруч» лише за блок: хлопок, що
      почався в самому кінці блоку, у спектрі цього блоку ще не видно
      (вікно Ганна його гасить), а гучність уже підскочила — і перша
      половина власного хлопка відкидала всю серію. Удар прийняли в межах
      130 мс — це той самий удар.  */
  if(_fgCandT && now - _fgCandT > 130){ _fg[_fgPos] = { _fgCandT, _fgCandLvl }; _fgPos = (_fgPos + 1) & 7; _fgCandT = 0; }
  /*  Свій звук — по смугах. Радіо знає, що грає (правий слот — вихід ЦАП):
      той самий спектр рахуємо й для нього, а співвідношення «динамік →
      мікрофон» вчимо окремо для низу й верху. Удар мусить вибиватися над
      власним звуком саме у своїй смузі: бочка чи тарілки в пісні там уже є.  */
  float exLo = 99, exHi = 99;
  if(ref){
    for(int i = 0; i < BLK; i++){ fftRef[2*i] = (ref[i] / 32768.0f) * win[i]; fftRef[2*i+1] = 0; }
    dsps_fft2r_fc32(fftRef, BLK);
    dsps_bit_rev_fc32(fftRef, BLK);
    float rlo = 0, rhi = 0;
    for(int b = 2; b <= 224; b++){
      float re = fftRef[2*b], im = fftRef[2*b+1], e2 = re*re + im*im;
      if(b <= 48) rlo += e2;
      if(b >= 64) rhi += e2;
    }
    /*  Кожна смуга вчиться окремо й лише там, де свій звук справді є (у
        станції без верхів верх так і не вивчиться — і не треба: удару там
        немає з чим плутатись). Раніше вчились лише обидві разом, а на
        кожному тихому місці пісні навчання скидалось — і поки воно
        тривало, відкидалось усе: хлопки під музику не проходили зовсім.  */
    const float E_MIN = 1e-9f;
    float dLo = 10.0f * log10f((lo + 1e-12f) / (rlo + 1e-12f));
    float dHi = 10.0f * log10f((hi + 1e-12f) / (rhi + 1e-12f));
    bool calm = !(onN && now - onT[onN-1] < 300);
    if(rlo > E_MIN){
      if(_cNLo < 20){ _cLo += (dLo - _cLo) / (_cNLo + 1); _cNLo++; }
      else if(calm && dLo - _cLo < 6) _cLo += (dLo - _cLo) * 0.03f;
      exLo = _cNLo >= 20 ? dLo - _cLo : -99;
    }
    if(rhi > E_MIN){
      if(_cNHi < 20){ _cHi += (dHi - _cHi) / (_cNHi + 1); _cNHi++; }
      else if(calm && dHi - _cHi < 6) _cHi += (dHi - _cHi) * 0.03f;
      exHi = _cNHi >= 20 ? dHi - _cHi : -99;
    }
  }
  /*  Поріг підлаштовується під кімнату: середнє й розкид звичайного фону.
      Удар займає два-три блоки (32 мс кожен) — у статистику фону не йде
      ні сам удар, ні 150 мс після нього: інакше хвіст першого хлопка
      піднімав поріг, і третій губився. Після гучного (музика стихла)
      поріг опускається вчетверо швидше, ніж піднімається.  */
  uint8_t sens = clapOn ? extras.s.clapSens : extras.s.knockSens;
  float k = sens == 1 ? 7.0f : sens == 2 ? 3.0f : 4.5f;   /* низька / висока / середня */
  float thr = onMu + k * onDev;
  bool outlier = onDev > 0 && val > thr;
  bool recent = onN && now - onT[onN-1] < 250;
  /*  Статистика фону вчиться завжди: звичайне — звично, «викид» — повільно
      й обрізаним. Раніше викиди не вчились зовсім, і поріг, вивчений у тиші
      (0,11), так і лишався, коли фон став більшим (2,5–3,5): кожен блок був
      «викидом», і поріг більше не рухався.  */
  if(!recent){
    float vc = outlier ? thr : val;
    float r = !outlier ? (val < onMu ? 0.12f : 0.03f) : 0.01f;
    onMu  += (vc - onMu) * r;
    onDev += (fabs(vc - onMu) - onDev) * (outlier ? 0.01f : 0.03f);
  }
  /*  Удар — або різка зміна спектра, або стрибок гучності на 10 дБ за 32 мс
      (хлопок здалеку в залі дає малу зміну спектра, але чіткий стрибок).  */
  bool jump = (int)_lvl - (int)_prevLvl >= (sens == 1 ? 14 : sens == 2 ? 7 : 10);
  /*  Досить гучний: на 12 дБ над фоном кімнати і на 8 дБ над тлом останніх
      секунд, не тихіший за −62 дБ. Виміряно на хлопках у залі: −34…−51 дБ
      при фоні −56…−63.  */
  bool loud = _lvl > _noise + (sens == 1 ? 16 : sens == 2 ? 9 : 12) && _lvl > _bgDb + 8.0f && _lvl > -62;
  /*  Вид — за «центром ваги» спектра удару: хлопок у долоні — близько
      1–2,5 кГц, стук по корпусу чи столу — сотні герц.  */
  float centre = cenD > 0 ? (float)(cenN / cenD) * MIC_FS / BLK : 0;
  HitBlk cur = { now, val, thr, lo, hi, centre, (float)cenD, exLo, exHi, _excess, _lvl, loud, ref != nullptr,
                 tot > 0 ? (float)(cenD / tot) : 0 };
  /*  Удар оцінюємо за трьома блоками поспіль. Почався в самому кінці блоку —
      гучність уже стрибнула, а спектр іще порожній (вікно Ганна гасить
      краї): вид визначався за тишею. Живий хлопок власника розтягнувся на три
      блоки, і тіло удару в третьому вже було «зарано». Тепер перший блок
      чекає двох наступних, а вид і «своє» беремо з того, де енергії удару
      найбільше.  */
  if(_hpN && now - _hp[0].t > 130) _hpN = 0;
  if(!_hpN){
    if(!outlier && !jump) return;
    _hp[0] = cur; _hpN = 1;
    /*  Що звучало перед ним: гучні блоки за ≈290 мс (9 блоків до цього) і
        кадри мови за ≈480 мс (16 кадрів, без останнього — там сам удар).  */
    /*  Хвіст і відлуння попереднього прийнятого удару (260 мс) — не «кімната
        звучала»: інакше другий хлопок серії відкидався б через перший.  */
    uint32_t last = onN ? onT[onN-1] : 0;
    auto afterHit = [&](uint32_t bt){ return onN && bt >= last && bt - last < 260; };
    uint8_t pre = 0;
    for(uint8_t i = 2; i <= 10; i++){
      uint8_t k = (lvlPos + 16 - i) & 15;
      if(lvlRing[k] > nzRing[k] + 10 && !afterHit(btRing[k])) pre++;
    }
    _hpPre = pre;
    uint8_t v = 0;
    for(uint8_t j = 1; j <= 16; j++){
      uint32_t age = j * 30;
      if(((vadRing >> j) & 1U) && !(now >= age && afterHit(now - age))) v++;
    }
    _hpVad = v;
    return;
  }
  _hp[_hpN++] = cur;
  if(_hpN < 3) return;
  _hpN = 0;
  /*  Наступний блок беремо, лише коли удар справді там (утричі більше енергії):
      інакше — перший. У стуку з маленького динаміка другий блок повний
      призвуків, і за ним стук «ставав» хлопком.  */
  uint8_t bi = 0;
  for(uint8_t i = 1; i < 3; i++) if(_hp[i].en > _hp[bi].en) bi = i;
  if(bi && _hp[bi].en <= _hp[0].en * 3.0f) bi = 0;
  const HitBlk& b = _hp[bi];
  uint32_t t = _hp[0].t;
  loud = _hp[0].loud || _hp[1].loud || _hp[2].loud;
  int8_t lvl = _hp[0].lvl;
  for(uint8_t i = 1; i < 3; i++) if(_hp[i].lvl > lvl) lvl = _hp[i].lvl;
  /*  не частіше ніж раз на 220 мс: у залі за хлопком ідуть відлуння через
      150–200 мс, а хлопати швидше за ~4 рази на секунду людина не встигає  */
  bool apart = onN == 0 || t - onT[onN-1] > 220;
  /*  Верхи й різкість — за всіма трьома блоками удару разом: у стуку клацання
      часто в одному блоці, а низьке тіло в іншому, і за одним блоком стук
      виглядав як склад без верхів.  */
  float sLo = 0, sHi = 0, vMax = 0;
  for(uint8_t i = 0; i < 3; i++){ sLo += _hp[i].lo; sHi += _hp[i].hi; if(_hp[i].val > vMax) vMax = _hp[i].val; }
  float hl = sLo > 1e-6f ? sHi / sLo : 99.0f;
  float vRel = _hp[0].thr > 1e-6f ? vMax / _hp[0].thr : 99.0f;
  /*  Вид — за центром спектра й верхами разом: хлопок — центр вище 650 Гц і
      помітні верхи; різкий удар без верхів — стук, навіть коли клацання
      кісточки підняло центр вище 650 Гц (раніше такий стук ставав «хлопком»
      без верхів і відкидався як мова).  */
  uint8_t kind = (b.centre > 650.0f && hl >= 0.08f) ? 1 : 2;
  /*  Мова — не хлопок. Склади розмови дають такі самі стрибки гучності, і
      розмова в кімнаті вмикала зупинене радіо («2 хлопки», «2 стуки»). Але
      в складі майже немає верхів: 2–7 кГц до низу 0,003–0,04, а в живому
      хлопку власника — від 0,077 (у записах 0,18–3,4). Стук по корпусу може
      бути й без верхів, тоді він мусить бути різким: сила зміни спектра хоч
      удвічі над порогом (у складах — 0,6–1,5 порога).  */
  bool sharp = vRel >= 2.0f;
  /*  Стук без помітних верхів пропускаємо лише різкий (2,5 порога) і з
      клацанням (верхи хоч 0,03 низу): склади, що проходили як «стук», мали
      0,002–0,025 і 2–3 порога — два такі за 288 мс ледь не стали жестом.  */
  bool crisp = kind == 1 || (hl >= 0.03f && vRel >= 4.0f) || vRel >= 6.0f;   /* склади мови — до 2,9 порога */
  /*  У тиші (радіо мовчить) удар ще й мусить бути різким: вибухові приголосні
      («п», «т», «к») мають верхи, але зміна спектра в них слабка — 1,0–2,9
      порога проти десятків у хлопку. Під музикою різкість не питаємо: там
      хлопок ловить стрибок гучності, а мову відсікає «своє».  */
  if(!b.ref && !sharp) crisp = false;
  bool speech = !crisp;
  /*  Гул — не стук. Кроки, посунутий стіл, поштовх корпусу дають глухий удар
      з центром спектра 70–150 Гц: гучність стрибає, а в смузі удару енергії
      майже нема. Саме такі два «стуки» вмикали зупинене радіо. Справжній стук
      по столу чи корпусу — 250–600 Гц, хлопок — вище 650 Гц.  */
  /*  Друга ознака гулу — енергія лежить нижче смуги удару: у стуку й хлопку в
      смузі 60 Гц–7 кГц більша частина спектра (виміряно 0,47–1,0), у гулі
      кімнати 0,13–0,15, навіть коли центр спектра вище 180 Гц.  */
  bool rumble = b.centre < 180.0f || b.frac < 0.35f;
  /*  удар у пісні радіо — не команда. У низу поріг вищий: на великій гучності
      маленький динамік спотворює бас, і цих спотворень в опорному сигналі немає  */
  float exBand = kind == 1 ? b.exHi : b.exLo;      /* 99 — свого звуку в цій смузі немає */
  float excess = _hp[0].excess;
  for(uint8_t i = 1; i < 3; i++) if(_hp[i].excess > excess) excess = _hp[i].excess;
  bool notOwn = !b.ref || exBand >= 99.0f
             || (exBand > -99.0f ? exBand > (kind == 1 ? 10.0f : 12.0f)
                                 : excess > 10.0f);       /* смуга ще не вивчена — за загальним рівнем */
  /*  увімкнено лише одне — удар іншого виду теж рахуємо: центр спектра
      у залі гуляє, а людина знає, що плескає  */
  if(kind == 1 && !clapOn) kind = knockOn ? 2 : 0;
  if(kind == 2 && !knockOn) kind = clapOn ? 1 : 0;
  bool accept = loud && notOwn && apart && kind && !_quietEdge && !rumble && !speech;
  if(_dbg) Serial.printf("##ONSET#\t%u центр=%.0fГц val=%.2f поріг=%.2f низ=%.2f верх=%.2f доля=%.2f в/н=%.3f перед=%u рівень=%d фон=%d тло=%.0f понад=%.0f/%.0f вид=%u блок=%u %s%s%s%s%s%s\n",
                         (unsigned)t, b.centre, b.val, b.thr, b.lo, b.hi, b.frac, hl, (unsigned)_hpPre, (int)lvl, (int)_noise, _bgDb, b.exLo, b.exHi, kind, (unsigned)(bi + 1),
                         loud ? "" : "тихо ", notOwn ? "" : "своє ", apart ? "" : "зарано ", rumble ? "гул " : "", speech ? "мова " : "", _quietEdge ? "звук вмикається " : accept ? "УДАР" : "");
  if(accept){
    if(onN == 4){ for(int i = 0; i < 3; i++){ onT[i] = onT[i+1]; onKind[i] = onKind[i+1]; onLvl[i] = onLvl[i+1]; } onN = 3; }
    onT[onN] = t; onKind[onN] = kind; onLvl[onN] = lvl; onN++;
    if(_fgCandT && t - _fgCandT < 130) _fgCandT = 0;
  }else if(loud && !_quietEdge && !(onN && t - onT[onN-1] < 250)){
    /*  гучний удар, що не пішов у жест (чужий, свій чи іншого виду), — жест
        навколо нього вже не жест: так ударні в музиці не стають командами  */
    if(!_fgCandT){ _fgCandT = t; _fgCandLvl = lvl; } else if(lvl > _fgCandLvl) _fgCandLvl = lvl;
  }
}

/*  Ритм — правила pector: між ударами не більше 0,45 с, для трійки ще й
    рівний крок (розбіжність до 0,1 с). Рішення — через 0,5 с тиші після
    останнього удару, щоб дві й три не плутались.  */
void YoMic::_pattern(uint32_t now){
  if(!onN || now - onT[onN-1] < 850) return;          /* рішення — коли 0,85 с нових ударів нема */
  /*  Серія — удари з проміжками до 0,8 с (виміряно: люди плескають із
      кроком 0,3–0,8 с). Вид серії — за більшістю ударів: у залі центр
      спектра хлопка гуляє, і «хлопок, стук» не має рвати жест.  */
  uint8_t cnt = 1, claps = onKind[onN-1] == 1, knocks = onKind[onN-1] == 2;
  for(int i = onN - 1; i > 0; i--){
    if(onT[i] - onT[i-1] > 780) break;                 /* час — у блоках по 32 мс: 768 ще серія, 800 — ні */
    cnt++;
    if(onKind[i-1] == 1) claps++; else knocks++;
  }
  /*  Жест — окрема серія: за 0,8 с до першого удару й між ударами — жодного
      стороннього удару, гучного хоч на рівні серії мінус 10 дБ; після
      останнього — лише майже такий самий гучний (мінус 4 дБ). Раніше рахувався
      будь-який «гучний»: у тиші відлуння хлопка й клацання підсилювача через
      300 мс гасили серію — хлопки приймались через раз, трійка — рідко.  */
  uint32_t first = onT[onN - cnt], last = onT[onN - 1];
  int8_t smin = 127;
  for(int i = onN - cnt; i < onN; i++) if(onLvl[i] < smin) smin = onLvl[i];
  for(uint8_t k = 0; k < 8; k++){
    const Foreign& f = _fg[k];
    if(!f.t || f.t + 800 < first) continue;
    bool within = f.t <= last + 250;
    if((within && f.lvl >= smin - 10) || (!within && f.lvl >= smin - 4)){
      if(_dbg) Serial.printf("##ONSET#\tрішення: серію з %u ударів відкинуто — поруч інший удар %d дБ (%+d мс від першого, серія від %d дБ)\n",
                             (unsigned)cnt, (int)f.lvl, (int)((int32_t)f.t - (int32_t)first), (int)smin);
      onN = 0;
      return;
    }
  }
  uint8_t kind = claps >= knocks ? 1 : 2;
  if(kind == 1 && !extras.s.clapOn) kind = 2;
  if(kind == 2 && !extras.s.knockOn) kind = 1;
  if(cnt >= 3){
    /*  трійка — з рівним кроком: розбіжність до 40 % кроку  */
    uint32_t d1 = onT[onN-1] - onT[onN-2], d2 = onT[onN-2] - onT[onN-3];
    uint32_t dm = d1 > d2 ? d1 : d2;
    if((d1 > d2 ? d1 - d2 : d2 - d1) * 10 > dm * 4) cnt = 2;
  }
  uint8_t g = MG_NONE;
  if(cnt == 2) g = kind == 1 ? MG_CLAP2 : MG_KNOCK2;
  else if(cnt >= 3) g = kind == 1 ? MG_CLAP3 : MG_KNOCK3;
  if(_dbg) Serial.printf("##ONSET#\tрішення: %u удари виду %u -> жест %u\n", (unsigned)cnt, (unsigned)kind, (unsigned)g);
  /*  Чим була серія — завжди, не лише з відладкою: інакше «само ввімкнулось»
      нема з чого розбирати (хлопок це був чи стук дверей).  */
  if(g){
    int w = 0;
    for(int i = onN - cnt; i < onN && w < (int)sizeof(_lastSeries) - 24; i++)
      w += snprintf(_lastSeries + w, sizeof(_lastSeries) - w, "%s%s %d дБ %u мс", i > onN - cnt ? ", " : "",
                    onKind[i] == 1 ? "хлоп" : "стук", (int)onLvl[i], (unsigned)(i > onN - cnt ? onT[i] - onT[i-1] : 0));
  }
  if(g){ _gesture = g; _heard = g; _heardMs = millis(); }
  onN = 0;
}

/*  ---------- самоперевірка тракту: динамік → повітря й корпус → мікрофон ---------- */
static const uint16_t SW_HZ[YoMic::SWEEP_N] = {
  63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000,
  1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000 };

uint16_t YoMic::sweepHz(uint8_t i){ return i < SWEEP_N ? SW_HZ[i] : 0; }

/*  Кличе цикл радіо (там само, де плеєр міняє частоту), тож перемкнути
    такти на 44,1 кГц тут безпечно: після ввімкнення вони на 16 кГц, а з
    ними вище 8 кГц не зміряти.  */
const char* YoMic::sweepStart(int8_t dbfs, bool amp, uint32_t mask, bool dsp){
  if(!_task)            return "мікрофон не запущено";
  /*  Зовнішній ЦАП — лише через обробку (dsp): там гучність тонів — людська чи «під кімнату» (_testGain).
      Сирий тон пішов би на чужий підсилювач на −12 дБ від повної шкали — на 20–40 дБ гучніше за музику.  */
  if(extras.s.dac && !dsp) return "зовнішній ЦАП: лише через обробку (dsp)";
  if(_swState == 1 || _swReq) return "уже міряю";
  if(player.isRunning()) return "спершу зупиніть звук";
  if(dbfs > -6)  dbfs = -6;
  if(dbfs < -40) dbfs = -40;
  _swLevel = dbfs;
  _swAmp = amp;
  _swDsp = dsp;
  _swMask = mask ? mask : 0x1FFFFFF;
  if(player.getSampleRate() != 44100) player.forceSampleRate(44100);
  _swStop = false;
  _swState = 1; _swPos = 0;
  _swReq = true;
  return nullptr;
}

/*  Амплітуда частоти f у записі (Гёрцель із вікном Ганна), частка повної шкали.  */
static float toneAmp(const float* x, size_t n, double wsum, float f, float fs){
  double c = 2.0 * cos(2.0 * M_PI * f / fs), s1 = 0, s2 = 0;
  for(size_t i = 0; i < n; i++){ double s = x[i] + c * s1 - s2; s2 = s1; s1 = s; }
  double p = s1 * s1 + s2 * s2 - c * s1 * s2;
  return (float)(2.0 * sqrt(p > 0 ? p : 0) / wsum);
}

static float toDb(float a){ return a > 1e-7f ? 20.0f * log10(a) : -140.0f; }

void YoMic::_sweep(int16_t* raw){
  const uint32_t MEMPS = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  const size_t FR = 256;                                   /* кадрів за один обмін */
  uint32_t fs = player.getSampleRate();
  if(fs < 8000) fs = 44100;
  _swRate = fs;
  for(int i = 0; i < SWEEP_N; i++){ _swDb[i] = _swNoise[i] = _swH2[i] = _swH3[i] = NAN; }
  _swPeak = 0;
  size_t capN = fs / 4;                                    /* 250 мс на аналіз */
  float*   cap = (float*)heap_caps_malloc(capN * sizeof(float), MEMPS);
  float*   sil = (float*)heap_caps_malloc(capN * sizeof(float), MEMPS);
  float*   hw  = (float*)heap_caps_malloc(capN * sizeof(float), MEMPS);
  int16_t* out = (int16_t*)heap_caps_malloc(FR * 4, MEMPS);
  if(!cap || !sil || !hw || !out){
    free(cap); free(sil); free(hw); free(out);
    _swState = 3; return;
  }
  double wsum = 0;
  for(size_t i = 0; i < capN; i++){ hw[i] = 0.5f - 0.5f * cos(2.0 * M_PI * i / (capN - 1)); wsum += hw[i]; }

  const float A = 32767.0f * pow(10.0f, _swLevel / 20.0f);
  const size_t fade = fs / 100;                            /* 10 мс наростання й спаду — без клацань */
  double phase = 0;
  bool ok = true;

  /*  Грати frames кадрів тону f (0 — тиша) і водночас записувати мікрофон;
      записані кадри з номерами [from, from+capN) кладемо в dst.  */
  auto pump = [&](float f, size_t frames, float* dst, size_t from) -> bool {
    double step = 2.0 * M_PI * f / fs;
    for(size_t done = 0; done < frames; ){
      if(_swStop || player.isRunning()) return false;
      size_t k = frames - done < FR ? frames - done : FR;
      for(size_t j = 0; j < k; j++){
        size_t t = done + j;
        float g = 1.0f;
        if(t < fade) g = (float)t / fade;
        else if(frames - t < fade) g = (float)(frames - t) / fade;
        int16_t v = f > 0 ? (int16_t)(A * g * sin(phase)) : 0;
        if(f > 0){ phase += step; if(phase > 2.0 * M_PI) phase -= 2.0 * M_PI; }
        if(_swDsp){
          /*  той самий шлях, яким іде радіо: еквалайзер, захист, гучність  */
          int16_t smp[2] = { v, v };
          uint32_t o = yoDsp.process(smp);
          out[2*j] = (int16_t)(o >> 16); out[2*j+1] = (int16_t)(o & 0xFFFF);
        }else{
          out[2*j] = v; out[2*j+1] = v;
        }
      }
      size_t w = 0, got = 0;
      if(_swAmp) extras.extAmpKick(false);
      i2s_write(I2S_NUM_0, out, k * 4, &w, pdMS_TO_TICKS(300));
      { YoI2sGuard lk; i2s_read(I2S_NUM_0, raw, k * 4, &got, pdMS_TO_TICKS(300)); }
      for(size_t j = 0; j < got / 4; j++){
        size_t t = done + j;
        if(dst && t >= from && t < from + capN){
          dst[t - from] = raw[2*j] / 32768.0f * hw[t - from];
          int16_t pk = raw[2*j] < 0 ? -raw[2*j] : raw[2*j];
          if(pk > _swPeak) _swPeak = pk;                       /* упор АЦП — ознака спотворення */
        }
      }
      done += k;
    }
    return true;
  };

  /*  старі записи з буферів приймання — геть  */
  for(int i = 0; i < 16; i++){
    size_t got = 0;
    { YoI2sGuard lk; i2s_read(I2S_NUM_0, raw, FR * 4, &got, 0); }
    if(!got) break;
  }
  /*  На час тонів піднімаємо пріоритет задачі. Звичайно вона слухає з
      найнижчим (0) — і цього досить, бо переважно спить на i2s_read. Але під
      час заміру вона має без упину готувати відліки: «під кімнату» жене тон
      ще й через усю обробку (еквалайзер, захист динаміка) — це близько 30%
      процесора, і на нульовому пріоритеті її щомиті витісняли Wi-Fi, екран,
      DLNA та AirPlay. Тон виходив рваний — на слух «притормаживание зі
      скрипом». Повертаємо пріоритет наприкінці.  */
  const UBaseType_t prioWas = uxTaskPriorityGet(nullptr);
  vTaskPrioritySet(nullptr, 6);
  /*  вбудований підсилювач — лише коли звук іде на вбудований динамік: із зовнішнім ЦАП його лінія даних
      відв'язана (applyDac), і ввімкнений підсилювач дав би хіба клацання й шум  */
  if(MUTE_PIN != 255) digitalWrite(MUTE_PIN, (_swAmp && !extras.s.dac) ? !MUTE_VAL : MUTE_VAL);
  if(_swAmp) extras.extAmpKick(false); else extras.extAmp(false);               /* і зовнішній, якщо звук іде на нього */
  /*  Фора буферу передачі. Далі запис і читання йдуть у лок-степі (записали
      кадр — стільки ж прочитали), тож без фори буфер передачі весь час на нулі:
      будь-яка затримка планувальника рве тон на шматки по 6 мс, і на слух це
      не тон, а скрип. Наливаємо наперед 80 мс тиші — аналіз однаково
      починається з 300-ї мс тону, тож зсув нічого не псує.  */
  {
    memset(out, 0, FR * 4);
    const size_t prime = (size_t)(fs * 8 / 100) / FR;
    for(size_t i = 0; i < prime && !_swStop; i++){
      size_t w = 0;
      if(_swAmp) extras.extAmpKick(false);
      i2s_write(I2S_NUM_0, out, FR * 4, &w, pdMS_TO_TICKS(300));
    }
  }
  ok = pump(0, fs / 5, nullptr, 0)                          /* 200 мс: підсилювач устоявся */
    && pump(0, fs * 3 / 10, sil, fs / 20);                  /* фон кімнати */
  for(uint8_t i = 0; ok && i < SWEEP_N; i++){
    float f = SW_HZ[i];
    if(f < fs * 0.45f && (_swMask >> i & 1)){
      phase = 0;
      /*  600 мс тону; аналізуємо з 300-ї мс — буфери передачі й приймання
          разом затримують звук менш ніж на 200 мс  */
      memset(cap, 0, capN * sizeof(float));
      ok = pump(f, fs * 6 / 10, cap, fs * 3 / 10) && pump(0, fs / 10, nullptr, 0);
      if(!ok) break;
      float a = toneAmp(cap, capN, wsum, f, fs);
      _swDb[i]    = toDb(a);
      _swNoise[i] = toDb(toneAmp(sil, capN, wsum, f, fs));
      if(2 * f < fs * 0.48f) _swH2[i] = toDb(toneAmp(cap, capN, wsum, 2 * f, fs)) - _swDb[i];
      if(3 * f < fs * 0.48f) _swH3[i] = toDb(toneAmp(cap, capN, wsum, 3 * f, fs)) - _swDb[i];
    }
    _swPos = i + 1;
  }
  i2s_zero_dma_buffer(I2S_NUM_0);
  if(MUTE_PIN != 255 && !player.isRunning()) digitalWrite(MUTE_PIN, MUTE_VAL);
  if(!player.isRunning()) extras.extAmp(false);
  free(cap); free(sil); free(hw); free(out);
  vTaskPrioritySet(nullptr, prioWas);
  _swState = ok ? 2 : 3;
}

/*  ---------- що робити з почутим (головний цикл) ---------- */

static const char* const ACT_NAME[MA_N] = { "типово", "нічого", "пауза / грати", "наступна", "попередня",
                                            "гучніше", "тихіше", "екран", "обране 1" };

const char* YoMic::actionName(uint8_t a){ return a < MA_N ? ACT_NAME[a] : ""; }

uint8_t YoMic::actionFor(MicGesture g){
  const ExtStore& e = extras.s;
  uint8_t a = g == MG_CLAP2 ? e.clap2 : g == MG_CLAP3 ? e.clap3 : g == MG_KNOCK2 ? e.knock2 : e.knock3;
  if(a == MA_DEFAULT || a >= MA_N) a = (g == MG_CLAP2 || g == MG_KNOCK2) ? MA_TOGGLE : MA_NEXT;
  return a;
}

void YoMic::injectGesture(uint8_t g){ if(g >= MG_CLAP2 && g <= MG_KNOCK3){ _gesture = g; _heard = g; _heardMs = millis(); } }

const char* YoMic::gestureName(uint8_t g){
  switch(g){
    case MG_CLAP2: return "2 хлопки";
    case MG_CLAP3: return "3 хлопки";
    case MG_KNOCK2: return "2 стуки";
    case MG_KNOCK3: return "3 стуки";
  }
  return "";
}

void YoMic::loop(){
  uint32_t now = millis();
  MicGesture g = takeGesture();
  /*  Дотик до екрана — теж стук по корпусу, і мікрофон його чує. Набір
      пароля ставав «двома стуками»: радіо вмикало станцію, без мережі
      шість секунд чекало з'єднання, і весь цей час екран не відповідав —
      літери й OK губились. Тож поки відкрите меню й кілька секунд після
      дотику жести не рахуються.  */
  if(g != MG_NONE){
    bool menu = false;
  #ifdef USE_YOMENU
    menu = yomenu.active();
  #endif
    if(menu || now - extras.lastTouchMs() < 3500){
      Serial.printf("##MIC#\t%s — не рахую: %s\n", gestureName(g), menu ? "відкрите меню" : "торкались екрана");
      g = MG_NONE;
    }else if(extras.alarmQuiet()){
      /*  радіо «вимкнене» й чекає будильника в темряві — звуки кімнати його не вмикають  */
      Serial.printf("##MIC#\t%s — не рахую: чекаю будильника\n", gestureName(g));
      g = MG_NONE;
    }
  }
  if(g != MG_NONE){
    sfx.play(SFX_GESTURE);                     /* чути, що жест прийнято */
    uint8_t a = actionFor(g);
    /*  Без мережі станцію не ввімкнеш — а спроба на секунди займає радіо.  */
    bool needNet = a == MA_TOGGLE || a == MA_NEXT || a == MA_PREV || a == MA_FAV1;
    if(needNet && config.getMode() == PM_WEB && network.status != CONNECTED && !player.isRunning()){
      Serial.printf("##MIC#\t%s → %s — немає мережі, не вмикаю\n", gestureName(g), actionName(a));
      a = MA_NONE;
    }
    Serial.printf("##MIC#\t%s → %s  (удари: %s)\n", gestureName(g), actionName(a), _lastSeries);
    switch(a){
      case MA_TOGGLE: player.toggle(); break;
      case MA_NEXT:   player.next(); break;
      case MA_PREV:   player.prev(); break;
      case MA_VOLUP:  player.stepVol(true); break;
      case MA_VOLDN:  player.stepVol(false); break;
      case MA_SCREEN: if(!extras.touchWake()) extras.screenOff(); break;
      case MA_FAV1:   extras.favPlay(0); break;
      default: break;
    }
  }
  if(!listening()){ extras.setPresenceDark(false); _earFrom = 0; return; }

  /*  Таймер сну слухає: у кімнаті N хвилин тихо — затихаємо, не чекаючи кінця.  */
  const ExtStore& e = extras.s;
  if(e.sleepEar && extras.sleepMinutes()){
    if(!_earFrom) _earFrom = now;
    uint32_t last = _lastRoom > _earFrom ? _lastRoom : _earFrom;
    uint32_t need = (uint32_t)(e.sleepEarMin ? e.sleepEarMin : 10) * minMs;
    if(now - last > need && extras.sleepSoon())
      Serial.printf("##MIC#\tу кімнаті тихо %u хв — затихаю\n", (unsigned)(need / minMs));
  }else _earFrom = 0;

  /*  Присутність: заговорили — екран прокидається; довго тихо — гасне.  */
  if(e.presWake && _lastVoice && _lastVoice != _wokeFor && now - _lastVoice < 1500){
    _wokeFor = _lastVoice;
    if(extras.screenDim()){ extras.touchWake(); Serial.println("##MIC#\tголос — будю екран"); }
  }
  if(e.presOff){
    if(!_presFrom) _presFrom = now;
    uint32_t last = _lastRoom;
    if(extras.lastTouchMs() > last) last = extras.lastTouchMs();
    if(_presFrom > last) last = _presFrom;
    bool dark = now - last > (uint32_t)e.presOff * minMs;
    if(dark && !extras.presenceDark()) Serial.println("##MIC#\tдовго тихо — гашу екран");
    if(!dark && extras.presenceDark()) Serial.println("##MIC#\tу кімнаті хтось є — екран світить");
    extras.setPresenceDark(dark);
  }else{ _presFrom = 0; extras.setPresenceDark(false); }
}

/*  ---------- перевірка жестів без рук ----------
    Радіо саме грає через динамік короткі хлопки (шумовий імпульс зі спадом
    ~8 мс, без низу) або стук (затухаючий тон 400 Гц), із заданим кроком, а
    мікрофон ловить їх звичайним шляхом: удари, ритм, дія. Лише коли плеєр
    стоїть — інакше нема чим грати й звук радіо змішався б із перевіркою.  */
const char* YoMic::simStart(uint8_t kind, uint8_t count, uint16_t gapMs){
  if(!listening())       return "мікрофон не слухає";
  if(player.isRunning()) return "спершу зупиніть звук";
  if(_tsKind || _swState == 1) return "уже зайнятий";
  if(count < 1) count = 1;
  if(count > 6) count = 6;
  _tsCount = count; _tsGap = gapMs < 60 ? 60 : gapMs; _tsPos = 0;
  if(MUTE_PIN != 255) digitalWrite(MUTE_PIN, !MUTE_VAL);
  extras.extAmpKick(false);
  _tsKind = kind >= 2 ? 3 : kind ? 2 : 1;
  return nullptr;
}

const char* YoMic::extStart(uint8_t kind, uint8_t count, uint16_t gapMs, int8_t peakDb){
  if(!listening()) return "мікрофон не слухає";
  if(_exKind)      return "уже зайнятий";
  _exCount = count < 1 ? 1 : count > 6 ? 6 : count;
  _exGap = gapMs < 60 ? 60 : gapMs;
  _exAmp = pow(10.0f, (peakDb > 0 ? 0 : peakDb) / 20.0f);
  _exPos = 0;
  _exKind = kind ? 2 : 1;
  return nullptr;
}

/*  Той самий хлопок чи стук, що й у micsim, але одразу в блок мікрофона
    (16 кГц) — з відлунням кімнати через 170 мс на 12 дБ тихіше.  */
void YoMic::_extMix(int16_t* x){
  const uint32_t fs = MIC_FS;
  uint32_t burst = fs * 40 / 1000, period = fs * _exGap / 1000, echo = fs * 170 / 1000;
  uint32_t total = period * (_exCount - 1) + burst + echo + fs;
  static uint32_t rnd = 777;
  static float prev = 0;
  for(int i = 0; i < BLK; i++){
    uint32_t t = _exPos + i;
    float v = 0;
    for(int e = 0; e < 2; e++){
      uint32_t d = e ? echo : 0;
      if(t < d) continue;
      uint32_t n = (t - d) / period, k = (t - d) % period;
      if(n >= _exCount || k >= burst) continue;
      float tt = (float)k / fs, g = e ? 0.25f : 1.0f;
      if(_exKind == 1){
        rnd = rnd * 1664525UL + 1013904223UL;
        v += g * ((int32_t)(rnd >> 16) - 32768) / 32768.0f * exp(-tt / 0.008f);
      }else{
        v += g * sin(2.0f * M_PI * 400.0f * tt) * exp(-tt / 0.015f);
      }
    }
    float y = _exKind == 1 ? (v - prev) * 0.5f : v;              /* хлопок — без низу */
    prev = v;
    int32_t o = x[i] + (int32_t)(y * _exAmp * 32767.0f);
    x[i] = (int16_t)(o > 32767 ? 32767 : o < -32768 ? -32768 : o);
  }
  _exPos += BLK;
  if(_exPos >= total) _exKind = 0;
}

void YoMic::_simChunk(int16_t* buf){
  const uint32_t FR = 256;
  uint32_t fs = player.getSampleRate(); if(fs < 8000) fs = 44100;
  uint32_t burst = fs * 40 / 1000, period = fs * _tsGap / 1000;
  /*  півтори секунди тиші наприкінці: у буферах I2S іще лежить не зіграний
      хвіст, і вимкнений раніше підсилювач «з'їдав» останній удар  */
  uint32_t lead = fs;                                           /* 1 с: підсилювач прокидається й клацає — це клацання не має впасти у вікно серії (0,8 с) */
  uint32_t total = lead + period * (_tsCount - 1) + burst + fs * 3 / 2;
  if(_tsKind == 3) total = lead + fs * 5 / 2 + fs * 3 / 2;          /* голос: 2,5 с «мови» */
  static uint32_t rnd = 22222;
  static int16_t prev = 0;
  for(uint32_t j = 0; j < FR; j++){
    uint32_t t = _tsPos + j;
    float v = 0;
    uint32_t n = t >= lead ? (t - lead) / period : 0xFFFF, k = t >= lead ? (t - lead) % period : 0;
    if(_tsKind == 3){
      /*  «голос»: гармоніки 140 Гц крізь три форманти (700, 1200, 2500 Гц),
          склади 4 на секунду — WebRTC VAD такий сигнал бере за мову  */
      if(t >= lead && t < lead + fs * 5 / 2){
        float tt = (float)(t - lead) / fs;
        float f0 = 140.0f + 10.0f * sin(2.0f * M_PI * 1.5f * tt);
        static float ph = 0;
        ph += 2.0f * M_PI * f0 / fs; if(ph > 2.0f * M_PI) ph -= 2.0f * M_PI;
        float acc = 0;
        for(int h = 1; h <= 20; h++){
          float f = f0 * h;
          float a = 1.0f / (1.0f + ((f - 700) / 150) * ((f - 700) / 150)) + 0.7f / (1.0f + ((f - 1200) / 200) * ((f - 1200) / 200))
                  + 0.4f / (1.0f + ((f - 2500) / 300) * ((f - 2500) / 300));
          acc += a * sin(ph * h);
        }
        float env = 0.5f * (1.0f - cos(2.0f * M_PI * 4.0f * tt));
        v = acc * env * 0.22f;
      }
    }else if(n < _tsCount && k < burst){
      float tt = (float)k / fs;
      if(_tsKind == 1){
        rnd = rnd * 1664525UL + 1013904223UL;
        float w = ((int32_t)(rnd >> 16) - 32768) / 32768.0f;
        v = w * exp(-tt / 0.008f) * 0.9f;
      }else{
        v = sin(2.0f * M_PI * 400.0f * tt) * exp(-tt / 0.015f) * 0.9f;
        /*  справжній стук кісточкою починається клацанням — 2 мс тихого шуму: гучніше — і центр спектра вже як у хлопка  */
        if(tt < 0.002f){ rnd = rnd * 1664525UL + 1013904223UL; v += ((int32_t)(rnd >> 16) - 32768) / 32768.0f * 0.4f * (1.0f - tt / 0.002f); }
      }
    }
    int16_t x = (int16_t)(v * 32767.0f);
    int16_t y = _tsKind == 1 ? (int16_t)((x - prev) / 2) : x;    /* хлопок — без низу */
    prev = x;
    buf[2*j] = y; buf[2*j+1] = y;
  }
  /*  лише записане рахується: відкинуті кадри раніше «з'їдали» наступний удар  */
  size_t w = 0;
  extras.extAmpKick(false);
  i2s_write(I2S_NUM_0, buf, FR * 4, &w, pdMS_TO_TICKS(600));
  _tsPos += w / 4;
  if(_tsPos >= total){
    _tsKind = 0;
    if(MUTE_PIN != 255 && !player.isRunning()) digitalWrite(MUTE_PIN, MUTE_VAL);
    if(!player.isRunning()) extras.extAmp(false);
  }
}
