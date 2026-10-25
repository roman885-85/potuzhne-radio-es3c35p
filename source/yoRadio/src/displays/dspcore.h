#ifndef dspcore_h
#define dspcore_h
#pragma once
/*  Екран: ES3C28P — ILI9341 2,8″ 320×240; ES3C35P — ST77922 3,5″ 320×480 (ландшафт 480×320).
    Драйвери інших екранів yoRadio прибрано.  */
#if DSP_MODEL==DSP_ILI9341
  #define TIME_SIZE           52
  #include "displayILI9341.h"
#elif DSP_MODEL==DSP_ST77922
  /*  ES3C35P — ST77922 3,5″ 320×480 по QSPI, радіо в ландшафті 480×320  */
  #define TIME_SIZE           52
  #include "displayST77922.h"
#else
  #error "ПОТУЖНЕ РАДІО збирається під ILI9341 (ES3C28P) або ST77922 (ES3C35P)"
#endif

#endif
