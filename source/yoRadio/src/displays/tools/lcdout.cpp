/*  Вивід смуг для ES3C28P (ILI9341 по SPI з DMA). Для ES3C35P — у displayST77922.cpp.  */
#include "lcdout.h"
#if DSP_MODEL==DSP_ILI9341
#include "../dspcore.h"
#include "spidma.h"
#include "../../m2/m2simd.h"

extern DspCore dsp;

static bool s_dma = false;        /* DMA працює (інакше — звичайний шлях Adafruit) */
static bool s_pending = false;    /* передача з буфера ще йде */

void lcdBegin(){
  s_dma = spidmaOk() || spidmaBegin();
  s_pending = false;
  dsp.startWrite();
}

bool lcdPush(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t* buf){
  if(w <= 0 || h <= 0) return false;
  const uint32_t n = (uint32_t)w * h;
  m2::sSwap16(buf, n);                         /* порядок байтів дисплея */
  if(s_pending){ if(!spidmaWait()) s_dma = false; s_pending = false; }
  dsp.setAddrWindow(x, y, w, h);
  if(s_dma && spidmaStart(buf, n * 2)){ s_pending = true; return true; }
  dsp.writePixels(buf, n, true, true);
  return false;
}

void lcdWait(){
  if(s_pending){ if(!spidmaWait()) s_dma = false; s_pending = false; }
}

void lcdEnd(){
  if(s_pending){ spidmaWait(); s_pending = false; }
  dsp.endWrite();
}

void* lcdScratch(size_t bytes){ return spidmaScratch(bytes); }
/*  ES3C28P: кадру в пам'яті немає — смуги йдуть у дисплей, перехід малює саме меню.  */
uint16_t* lcdFrameFor(int16_t x, int16_t y){ (void)x; (void)y; return nullptr; }
int16_t   lcdFrameStride(){ return 0; }
void      lcdMarked(int16_t x, int16_t y, int16_t w, int16_t h){ (void)x; (void)y; (void)w; (void)h; }
bool lcdSlideBegin(){ return false; }
void lcdSlideRun(int8_t dir, uint32_t ms){ (void)dir; (void)ms; }
#endif
