/*  Запасний розпакувальник картинок — stb_image v2.30 (Sean Barrett, public domain /
    MIT, github.com/nothings/stb; копія з дерева LVGL — sha256 d81d9528…). Узято, бо
    власні розпакувальники (PNG, ICO, JPEG з ПЗУ — TJpgDec) не знають прогресивного
    JPEG, PNG через рядок і ICO з палітрою, а значки станцій бувають саме такі
    (25.10.2026: «Радіо Марія» — прогресивний JPEG, «Голос Мира» — ICO 8 біт).
    Уся пам'ять — у PSRAM; стек задачі логотипа (16 КБ) вміщує його таблиці.  */
#include <Arduino.h>
#include "esp_heap_caps.h"

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_MAX_DIMENSIONS 4096
#define STBI_MALLOC(sz)       heap_caps_malloc((sz), MALLOC_CAP_SPIRAM)
#define STBI_REALLOC(p, sz)   heap_caps_realloc((p), (sz), MALLOC_CAP_SPIRAM)
#define STBI_FREE(p)          heap_caps_free(p)
#define STBI_ASSERT(x)        ((void)0)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "stb_image.h"
#pragma GCC diagnostic pop

/*  RGBA (4 байти на точку) у PSRAM; звільняти free(). Зображення понад ~0,6 Мп не
    беремо: прогресивний JPEG тримає ще й усі коефіцієнти (по 6 байтів на точку).  */
bool yoStbDecode(const uint8_t* d, size_t n, uint8_t** rgba, uint32_t* W, uint32_t* H){
  int x = 0, y = 0, c = 0;
  if(!d || n < 8 || !stbi_info_from_memory(d, (int)n, &x, &y, &c)) return false;
  /*  ≤ 0,4 Мп: розпакування йде без пауз, і задовге не дало б часу задачі простою  */
  if(x <= 0 || y <= 0 || (uint32_t)x * (uint32_t)y > 400000UL) return false;
  stbi_uc* p = stbi_load_from_memory(d, (int)n, &x, &y, &c, 4);
  if(!p) return false;
  *rgba = p; *W = (uint32_t)x; *H = (uint32_t)y;
  return true;
}

const char* yoStbWhy(){ return stbi_failure_reason(); }
