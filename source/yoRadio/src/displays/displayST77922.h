#ifndef displayST77922_h
#define displayST77922_h
/*  Екран плати ES3C35P — ST77922 320×480 по QSPI; радіо тримає його в ландшафті
    480×320 і повертає картинку саме (апаратного повороту в панелі немає).
    Малює все нове меню й головний екран (src/m2) через lcdout.h; цей клас —
    лише те, що треба решті радіо: старт, заливка, логотип, сон, інверсія.  */
#include "Arduino.h"
#include <Adafruit_GFX.h>
#include "fonts/bootlogo99x64.h"
#include "fonts/dsfont52.h"

typedef GFXcanvas16 Canvas;
#include "widgets/widgetsconfig.h"
#include "tools/utf8Rus.h"
#include "conf/displayST77922conf.h"

extern volatile bool g_dspWatch;       /* налагодження (див. commongfx.h) */
extern volatile bool g_m2Draw;

class DspCore : public Adafruit_GFX {
  public:
    DspCore();
    void initDisplay();
    void clearDsp(bool black = false);
    void drawPixel(int16_t x, int16_t y, uint16_t color) override;
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override;
    void fillScreen(uint16_t color) override;
    void drawLogo(uint16_t top);
    void flip();
    void invert();
    void sleep();
    void wake();
    void setSPISpeed(uint32_t hz);
    /*  Знімок екрана: рядок із «тіні» — копії всього, що пішло в панель (lcdShadow).
        Відеопам'ять ST77922 читається лише стиснутою (блоки по 4 пікселі).  */
    void dbgReadRow(uint16_t y, uint16_t w, uint16_t* out);
    inline void loop(bool force = false) { (void)force; }
    void printClock() {}
};

/*  Службове: панель і поворот  */
uint8_t  st77922Rot();                   /* 1 — USB праворуч (як на фото виробника), 3 — перевернуто */
uint32_t st77922Hz();
void     lcdShadow(bool on);             /* копія кадру в PSRAM для знімків екрана */
bool     lcdShadowOn();
const uint16_t* lcdFrame();               /* кадр у PSRAM (480×320) — для знімка без паузи звуку (scrq) */
void     lcdStats(bool reset);           /* скільки пішло в панель і скільки коштував поворот */
void     lcdRotPie(int8_t on);           /* 1 — поворот векторними інструкціями, 0 — звичайний код, -1 — показати */
bool     lcdRotCheck();                  /* звірити векторний поворот зі звичайним на випадкових смугах */
void     lcdTe(int8_t mode);             /* такт панелі: -1 показати, 0 вимк., 1 наростання, 2 спад, 3 як заміряно */
void     lcdSlideTest(int8_t dir);       /* зсув екрана самого на себе — зсуви й час кадрів */
/*  0x45 (номер рядка розгортки) ця панель не віддає — завжди 0 (перевірено 26.10.2026).  */
void     lcdRegRead(uint8_t reg, uint8_t len, uint8_t dummy);
void     lcdPanelCmd(uint8_t c);         /* команда панелі без параметрів (перевірка tsdark: 0x28, 0x10, 0x11, 0x29) */

#endif
