/*  Нове меню: кольори, розміри, шрифти.  */
#ifndef m2theme_h
#define m2theme_h
#include <Arduino.h>
#include "../core/options.h"
/*  Масштаб інтерфейсу. Сторінки малюють у «логічних» пікселях — сітці старого
    екрана заввишки 240, — а Gfx (m2gfx.cpp) переводить їх у пікселі панелі.
    ES3C28P: 1:1 (320×240). ES3C35P: 4/3 — логічно 360×240, панель 480×320.
    Так увесь інтерфейс — шрифти, кнопки, відступи — стає на третину більшим,
    у тих самих пропорціях, а сторінки переписувати не треба.  */
#if DSP_H >= 320
  #define UI_KN 4
  #define UI_KD 3
  #include "../displays/fonts/aa/m2TitleL.h"
  #include "../displays/fonts/aa/m2BigL.h"
  #include "../displays/fonts/aa/m2MidL.h"
  #include "../displays/fonts/aa/m2RowL.h"
  #include "../displays/fonts/aa/m2RowBL.h"
  #include "../displays/fonts/aa/m2SmL.h"
  #include "../displays/fonts/aa/m2SmBL.h"
  #include "../displays/fonts/aa/m2KeyL.h"
  #include "../displays/fonts/aa/m2ClockL.h"
  #include "../displays/fonts/aa/aaUI6L.h"
  #include "../displays/fonts/aa/aaUI26bL.h"
#else
  #define UI_KN 1
  #define UI_KD 1
  #include "../displays/fonts/aa/m2Title.h"
  #include "../displays/fonts/aa/m2Big.h"
  #include "../displays/fonts/aa/m2Mid.h"
  #include "../displays/fonts/aa/m2Row.h"
  #include "../displays/fonts/aa/m2RowB.h"
  #include "../displays/fonts/aa/m2Sm.h"
  #include "../displays/fonts/aa/m2SmB.h"
  #include "../displays/fonts/aa/m2Key.h"
  #include "../displays/fonts/aa/m2Clock.h"
  #include "../displays/fonts/aa/aaUI6.h"
  #include "../displays/fonts/aa/aaUI26b.h"
#endif

namespace m2 {

constexpr uint16_t RGB(uint8_t r, uint8_t g, uint8_t b){ return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }

/*  Темна сіро-синя гама замість чорного тла; жовтий — фірмовий, як на заставці.  */
const uint16_t C_BG     = RGB(12, 15, 21);
const uint16_t C_BGTOP  = RGB(20, 25, 35);
const uint16_t C_SURF   = RGB(27, 32, 42);     /* картки */
const uint16_t C_SURF2  = RGB(42, 48, 61);     /* кнопки на картках, доріжки */
const uint16_t C_LINE   = RGB(46, 53, 66);     /* роздільники */
const uint16_t C_TXT    = RGB(242, 242, 236);
const uint16_t C_TXT2   = RGB(140, 148, 162);  /* другорядне */
const uint16_t C_TXT3   = RGB(92, 100, 114);   /* вимкнене */
const uint16_t C_ACC    = 0xE68B;              /* жовтий радіо (#E3D25F) */
const uint16_t C_ACCTXT = RGB(40, 36, 16);     /* текст на жовтому */
const uint16_t C_KNOB   = RGB(250, 250, 248);
const uint16_t C_ORANGE = RGB(255, 138, 61);
const uint16_t C_BLUE   = RGB(77, 163, 255);
const uint16_t C_TEAL   = RGB(52, 211, 160);
const uint16_t C_VIOLET = RGB(176, 124, 255);
const uint16_t C_RED    = RGB(255, 84, 84);
const uint16_t C_GREY   = RGB(120, 128, 142);
const uint16_t C_PINK   = RGB(255, 99, 160);
const uint16_t C_GREEN  = RGB(70, 200, 110);
const uint16_t C_REC    = RGB(235, 60, 60);

/*  Панель (фізичні пікселі, плата: myoptions.h) і логічна сітка сторінок.  */
const int16_t PSW = DSP_W, PSH = DSP_H;
const int16_t SW = DSP_W * UI_KD / UI_KN, SH = DSP_H * UI_KD / UI_KN;
/*  Брудні ділянки екрана рахуються плитками 16×16 у пікселях ПАНЕЛІ:
    рядок плиток — біти uint32_t.  */
const int16_t TCOLS = (PSW + 15) / 16, TROWS = (PSH + 15) / 16;
static_assert(TCOLS <= 32, "рядок плиток має вміщатись у uint32_t");
/*  Логічні → пікселі панелі (до найближчого — краї сусідніх прямокутників
    збігаються), дробові — для згладжених фігур; і назад — для дотиків.  */
constexpr int16_t uiP(int32_t v){
  return UI_KN == UI_KD ? (int16_t)v
       : (int16_t)(v >= 0 ? (v * UI_KN + UI_KD / 2) / UI_KD : -((-v * UI_KN + UI_KD / 2) / UI_KD));
}
constexpr float   uiPf(float v){ return v * (float)UI_KN / (float)UI_KD; }
constexpr int16_t uiPfloor(int32_t v){ return (int16_t)(v >= 0 ? v * UI_KN / UI_KD : -((-v * UI_KN + UI_KD - 1) / UI_KD)); }
constexpr int16_t uiPceil(int32_t v){ return (int16_t)(v >= 0 ? (v * UI_KN + UI_KD - 1) / UI_KD : -((-v * UI_KN) / UI_KD)); }
constexpr int16_t uiL(int32_t v){ return UI_KN == UI_KD ? (int16_t)v : (int16_t)((v * UI_KD + UI_KN / 2) / UI_KN); }
const int16_t HDR = 40;                        /* шапка */
const int16_t CH = SH - HDR;                   /* висота вмісту */
const int16_t MX = 10;                         /* поля карток */
const int16_t CWID = SW - 2 * MX;              /* ширина картки */
const uint8_t R_CARD = 12, R_BTN = 10, R_BADGE = 7;

#if UI_KN != UI_KD
/*  шрифти вже в пікселях панелі (×4/3): Gfx міряє їх і перераховує ширину в логічні  */
#define F_TITLE (&m2TitleL)
#define F_BIG   (&m2BigL)
#define F_MID   (&m2MidL)
#define F_ROW   (&m2RowL)
#define F_ROWB  (&m2RowBL)
#define F_SM    (&m2SmL)
#define F_SMB   (&m2SmBL)
#define F_KEY   (&m2KeyL)
#define F_TINY  (&aaUI6L)
#define F_POP   (&aaUI26bL)
#define F_CLOCK (&m2ClockL)
#else
#define F_TITLE (&m2Title)
#define F_BIG   (&m2Big)
#define F_MID   (&m2Mid)
#define F_ROW   (&m2Row)
#define F_ROWB  (&m2RowB)
#define F_SM    (&m2Sm)
#define F_SMB   (&m2SmB)
#define F_KEY   (&m2Key)
#define F_TINY  (&aaUI6)
#define F_POP   (&aaUI26b)
#define F_CLOCK (&m2Clock)
#endif

}  // namespace m2
#endif
