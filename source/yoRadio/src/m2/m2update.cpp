/*  Нове меню й екран: оновлення з GitHub (extras/yoOta).
    - сторінка «Оновлення» в «Параметрах»;
    - екран ходу встановлення — поверх усього, хоч би що було відкрите;
    - пропозиція оновитись — на головному екрані (m2player.cpp).  */
#include "../core/options.h"
#include "m2pages.h"
#include "m2lang.h"
#include "m2update.h"
#include "../core/config.h"
#include "../displays/dspcore.h"
#include "../displays/tools/spidma.h"
#include "../displays/tools/lcdout.h"
#include "m2simd.h"
#include "../extras/yoOta.h"
#include "../extras/yoVersion.h"
#include "../extras/yoExtras.h"

extern DspCore dsp;

namespace m2 {

/*  =================== сторінка «Оновлення» =================== */
static const char* vUpdNote(){
  static char b[200];
  switch(ota.state()){
    case OTA_CHECKING: return "звертаюсь до GitHub…";
    case OTA_LATEST:   return ota.hasFile() ? "у радіо остання версія" : "випусків для цієї плати ще не було";
    case OTA_ERROR:    snprintf(b, sizeof(b), tr("не вийшло: %s"), tr(ota.error())); return b;
    case OTA_AVAILABLE: {
      /*  перший рядок опису й нагадування, що нічого не загубиться (сторінка — на один екран, окремого рядка нема)  */
      const char* s1 = ota.notes(); size_t k = 0;
      while(s1[k] && s1[k] != '\n' && k < 90) k++;
      if(!k) return "вийшла нова версія\nстанції й налаштування лишаються";
      memcpy(b, s1, k); b[k] = 0;
      snprintf(b + k, sizeof(b) - k, "\n%s", tr("станції й налаштування лишаються"));
      return b;
    }
    default: return ota.available() ? "вийшла нова версія" : "радіо саме перевіряє GitHub двічі на добу";
  }
}
static const char* vInstall(){ static char b[48]; snprintf(b, sizeof(b), tr("Встановити %s"), ota.latest()); return b; }
/*  Одна кнопка: є новіша — встановити, інакше — перевірити (сторінка на один екран, 03.11.2026).  */
static bool updReady(){ return ota.available() && !ota.installing(); }
static const char* vUpdBtn(){ return updReady() ? vInstall() : "Перевірити зараз"; }
/*  GitHub коротко — повне пояснення в рядку нижче  */
static const char* vGhShort(){
  if(ota.state() == OTA_CHECKING) return "…";
  if(!ota.latest()[0]) return "—";
  if(!ota.hasFile()) return "немає";
  return ota.latest();
}
static void verRow(Gfx& g, int16_t x, int16_t y, int16_t w, int16_t h){
  const int16_t hw = w / 2;
  g.fill(x + hw, y + 8, 1, h - 16, C_LINE);
  g.text(x + 14, y + h / 2 + 4, "У радіо", F_ROW, C_TXT, AL_L, hw - 80);
  g.text(x + hw - 12, y + h / 2 + 4, prVersion(), F_ROW, C_TXT2, AL_R, 60);
  g.text(x + hw + 14, y + h / 2 + 4, "GitHub", F_ROW, C_TXT, AL_L, hw - 80);
  g.text(x + w - 12, y + h / 2 + 4, vGhShort(), F_ROW, ota.available() ? C_ACC : C_TXT2, AL_R, 60);
}

/*  Чотири рядки на один екран (раніше з розділами й поясненнями — 352, треба було гортати).  */
static Item s_updItems[] = {
  [](){ Item i = iCustom(44, verRow); i.color = 1; return i; }(),
  iNote(vUpdNote, 36),
  iButton("Перевірити зараз", IC_REFRESH, [](){ if(updReady()){ ota.install(); M.closeNow(); } else ota.check(extras.s.otaBeta); }),
  iSwitch("Пробні версії", IC_CODE, C_ORANGE, [](){ return (int32_t)extras.s.otaBeta; },
          [](int32_t v){ extras.s.otaBeta = v ? 1 : 0; extras.changed(); ota.check(extras.s.otaBeta);
                         M.toast(v ? "пропонувати й пробні випуски" : "лише випуски для всіх"); }),
};

class UpdatePage : public ListPage {
  public:
    UpdatePage() : ListPage("Оновлення", s_updItems, sizeof(s_updItems) / sizeof(s_updItems[0])) {}
    void enter() override {
      s_updItems[2].enabled = [](){ return updReady() || !ota.busy(); };
      s_updItems[2].text = vUpdBtn;
      ListPage::enter();
      _sig = 0;
      if(!ota.busy() && (!ota.checkedAt() || millis() - ota.checkedAt() > 600000UL)) ota.check(extras.s.otaBeta);
    }
    void tick(uint32_t now) override {
      ListPage::tick(now);
      /*  рядок версій — власний: перемалювати, коли GitHub відповів  */
      uint32_t s = ota.state() * 7 + (ota.available() ? 1 : 0) + (ota.hasFile() ? 2 : 0);
      for(const char* p = ota.latest(); *p; p++) s = s * 31 + (uint8_t)*p;
      if(s != _sig){ if(_sig) M.inval(Rect(0, s_updItems[0].y, SW, s_updItems[0].hh)); _sig = s; }
    }
  private:
    uint32_t _sig = 0;
};
static UpdatePage s_update;
Page& pgUpdate = s_update;

/*  =================== екран ходу встановлення =================== */
static uint32_t s_sig = 0xFFFFFFFF;
static bool s_was = false;

bool otaViewActive(){ return ota.installing(); }

static void drawOta(Gfx& g){
  g.vgrad(0, 0, SW, 120, C_BGTOP, C_BG);
  g.fill(0, 120, SW, SH - 120, C_BG);
  g.text(SW / 2, 34, "Оновлення радіо", F_TITLE, C_TXT, AL_C);
  char b[96];
  snprintf(b, sizeof(b), "%s  →  %s", prVersion(), ota.latest() + (ota.latest()[0] == 'v' ? 1 : 0));
  g.text(SW / 2, 56, b, F_ROWB, C_ACC, AL_C);
  /*  кільце ходу  */
  const float cx = SW / 2, cy = 120;
  OtaState st = ota.state();
  /*  Один суцільний хід по всіх фазах, а не окреме кільце, що крутиться:
      підготовка й завантаження веб-файлів ідуть по мережі (ядро 0 зайняте
      Wi-Fi/TLS), і кільце, що оберталось за часом, смикалось на кожному
      пропущеному кадрі. Визначений відсоток, що лише наростає, читається як
      хід, а не як ривки.  */
  const uint8_t p = ota.progress();
  int overall;
  switch(st){
    case OTA_WEB:      overall = 3 + p * 12 / 100; break;    /* 3..15 */
    case OTA_FIRMWARE: overall = 15 + p * 81 / 100; break;   /* 15..96 */
    case OTA_VERIFY:   overall = 98; break;
    case OTA_DONE:     overall = 100; break;
    default:           overall = 3; break;                   /* OTA_PREPARE тощо */
  }
  g.arc(cx, cy, 38, 7, C_SURF2);
  g.arc(cx, cy, 38, 7, st == OTA_DONE ? C_TEAL : C_ACC, 0, 3.6f * overall);
  snprintf(b, sizeof(b), "%d%%", overall);
  g.text((int16_t)cx, (int16_t)cy + 8, b, F_MID, C_TXT, AL_C);
  g.text(SW / 2, 186, ota.stepName(), F_ROW, C_TXT, AL_C, SW - 20);
  if(st == OTA_FIRMWARE && ota.total()){
    snprintf(b, sizeof(b), tr("%.1f з %.1f МБ · %u КБ/с"), ota.done() / 1048576.0f, ota.total() / 1048576.0f, (unsigned)(ota.speed() / 1024));
    g.text(SW / 2, 206, b, F_SM, C_TXT2, AL_C);
  }
  g.text(SW / 2, 228, st == OTA_DONE ? "за мить радіо увімкнеться знову" : "не вимикайте радіо", F_SM, st == OTA_DONE ? C_TEAL : C_TXT2, AL_C);
}

void otaViewRender(){
  s_was = true;
  OtaState st = ota.state();
  uint32_t sig = (uint32_t)st * 1000 + ota.progress() + (ota.speed() / 4096) * 131;
  if(sig == s_sig) return;
  s_sig = sig;
  /*  підсвітка — повна: людина має бачити, що йде  */
  static bool lit = false;
  if(!lit){ lit = true; extras.pwmSet(extras.pwmTarget() ? extras.pwmTarget() : 180); }
  const int32_t STRIP = (int32_t)PSW * (LCD_PUSH_COPIES ? 16 : 32);   /* пікселі панелі */
  uint16_t* buf = (uint16_t*)lcdScratch((size_t)STRIP * 2);
  if(!buf) return;
  const int16_t RH = lcdRowsFor(PSW, STRIP);         /* 32 рядки на ES3C28P, 16 на ES3C35P */
  Gfx g;
  g_m2Draw = true;
  lcdBegin();
  for(int16_t y0 = 0; y0 < PSH; y0 += RH){
    int16_t h = y0 + RH > PSH ? PSH - y0 : RH;
    g.target(buf, 0, y0, PSW, h);
    drawOta(g);
    if(lcdPush(0, y0, PSW, h, buf)) lcdWait();      /* буфер один — дочекатись, перш ніж малювати наступну */
  }
  lcdEnd();
  g_m2Draw = false;
}

bool otaViewEnded(){
  if(s_was && !ota.installing()){ s_was = false; s_sig = 0xFFFFFFFF; return true; }
  return false;
}

}  // namespace m2
