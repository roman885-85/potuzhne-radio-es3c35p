#include "yoVersion.h"
#include "../core/options.h"      /* PR_BOARD — назва плати в мітці прошивки */
#if __has_include("yoBuild.h")
  #include "yoBuild.h"
#endif
#ifndef PR_VERSION
  #define PR_VERSION "1.0"
#endif
#ifndef PR_BUILD
  #define PR_BUILD   __DATE__ " " __TIME__
#endif

/*  Той самий рядок шукає сторінка у файлі прошивки. Змінювати разом із
    перевіркою в web/app.js (FW_MARK).  */
static const char kMarker[] = "POTUZHNE-RADIO-FW|" PR_BOARD "|" PR_VERSION "|" PR_BUILD "|";

const char* prVersion(){ return PR_VERSION; }
const char* prBuild(){ return PR_BUILD; }
const char* prMarker(){ return kMarker; }
