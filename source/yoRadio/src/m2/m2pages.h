/*  Нове меню: сторінки (оголошення для взаємних переходів).  */
#ifndef m2pages_h
#define m2pages_h
#include "m2ui.h"

namespace m2 {

extern Page& pgPult;
extern Page& pgSettings;
extern Page& pgScreen;
extern Page& pgAlarm;
extern Page& pgFav;
extern Page& pgSermons;
extern Page& pgInfo;
extern Page& pgPower;
extern Page& pgTz;
extern Page& pgEq;
extern Page& pgSound;
extern Page& pgRoom;
extern Page& pgMic;
extern Page& pgGest;
extern Page& pgPres;
extern Page& pgWifi;
extern Page& pgSaved;
extern Page& pgNet;       /* знайома мережа: підключитись / змінити пароль / забути */
extern Page& pgConnect;   /* хід підключення */
extern Page& pgKbd;
extern Page& pgDev;
extern Page& pgDevSnd;
extern Page& pgDac;
extern Page& pgDacInfo;
extern Page& pgNightFrom;
extern Page& pgNightTo;
extern Page& pgUpdate;
extern Page& pgStations;  /* список станцій / треків картки */
/*  Підсторінки довгих сторінок (03.11.2026: кожна сторінка налаштувань — на один екран, без прокрутки)  */
extern Page& pgSpeakers;  /* Параметри › Колонки: DLNA, AirPlay */
extern Page& pgSystem;    /* Параметри › Система: мова, пояс, автостарт */
extern Page& pgNight;     /* Екран › Нічний режим */
extern Page& pgBatLed;    /* Екран › Батарея й світлодіод */
extern Page& pgSleep;     /* таймер сну (утримати плитку «Сон») */
extern Page& pgSndVol;    /* Обробка › Гучність і баланс */
extern Page& pgSndSpk;    /* Обробка › Динамік */
extern Page& pgSndBass;   /* Обробка › Низ */
extern Page& pgGestKind;  /* Хлопки й стук › Хлопки / Стук */
extern Page& pgDevShow;   /* Розробник › Що показувати */
extern Page& pgDevHw;     /* Розробник › Обладнання */
extern Page& pgMute;      /* Розробник › Обладнання › MUTE підсилювача */
extern Page& pgLogos;     /* Розробник › Логотипи станцій */
extern Page& pgTouch;     /* Розробник › Сенсор */
extern Page& pgDebug;     /* Розробник › Налагодження */
extern Page& pgSplash;    /* Заставка й звуки › Заставка */
extern Page& pgSfx;       /* Заставка й звуки › Звуки подій */
extern Page& pgSfxVol;    /* Заставка й звуки › Гучність кожного звуку */
extern Page& pgWifiScan;  /* Wi-Fi › Мережі поруч */

/*  попросити список станцій (будь-яка задача); відкриває головний цикл — stationsPoll() у Menu::loop()  */
void stationsRequest();
void stationsPoll();
extern volatile bool m2RowCache;   /* кеш готових рядків списку (налагодження: m2cache) */

/*  клавіатура: куди писати, заголовок, що робити після «OK» (головний цикл)  */
void kbdOpen(char* target, size_t max, bool password, const char* title, void (*done)(bool ok));

/*  після закриття меню: 1 — відкрити список станцій, 2 — показати заставку  */
extern volatile uint8_t afterClose;

/*  Барабан часу: дві колонки, які крутяться пальцем.  */
struct Drum {
  int16_t top = 0;                  /* верх у координатах вмісту */
  int16_t h = 118;
  float   pos[2] = { 0, 0 };        /* поточне положення колонок (у кроках) */
  int16_t val[2] = { 0, 0 };        /* значення, яке показують (у кроках) */
  int16_t n[2] = { 24, 60 };        /* скільки кроків у колонці */
  bool    drag = false;
  uint8_t col = 0;
  int16_t y0 = 0; float p0 = 0; float vel = 0; uint32_t lastT = 0; int16_t lastY = 0;
  bool    snapping[2] = { false, false };
  const char* (*label)(uint8_t col, int16_t v) = nullptr;   /* як підписати значення */
  void draw(Gfx& g);
  bool tick(uint32_t now);          /* true — рухається (перемалювати) */
  void press(int16_t x, int16_t y);
  void move(int16_t y);
  void release();
  void setVal(uint8_t c, int16_t v){ val[c] = v; if(!drag) pos[c] = v; }
  int16_t at(uint8_t c) const;      /* найближче значення до поточного положення */
};

}  // namespace m2
#endif
