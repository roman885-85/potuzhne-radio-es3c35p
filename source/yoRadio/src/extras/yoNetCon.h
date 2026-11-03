/*
 *  Консоль налагодження по Wi-Fi — те саме, що USB-консоль, без шнура.
 *
 *  Усе, що радіо пише в Serial (журнал «##…», відповіді команд налагодження), іде
 *  ще й у кільце 64 КБ у PSRAM; під'єднаний по TCP (порт NETCON_PORT) клієнт отримує
 *  його наживо, а рядки, які він шле, — потрапляють туди ж, звідки команди бере
 *  USB-консоль (menu/yoDebug.cpp). Пишучі задачі мережу не чекають ніколи: повільний
 *  клієнт лише пропускає частину журналу.
 *
 *  Клієнт: tools/es3c35p/con.py --net [адреса] (типово potuzhne-<id>.local),
 *  знімки екрана — scr.py --net, прошивка — firmware/flash.sh --wifi.
 *  Свої команди каналу: «hist» — журнал із пам'яті (зокрема старт, якого по мережі
 *  ще не було чути), «bye» — від'єднатись. Під'єднатись можна лише з тієї ж мережі
 *  (та сама підмережа, що й у радіо); вимикач — «Розробник › Консоль по Wi-Fi».
 *
 *  Підміна Serial — тут же, внизу: файл вмикається з core/options.h, тож її бачить
 *  увесь код радіо (ядро Arduino й сторонні бібліотеки — ні).
 */
#ifndef yoNetCon_h
#define yoNetCon_h

#include <Arduino.h>

#define NETCON_PORT 2323

/*  Справжній порт, який підміняємо (так само, як вибирає HardwareSerial.h).  */
#if ARDUINO_USB_CDC_ON_BOOT
  #if ARDUINO_USB_MODE
    #define YO_REAL_SERIAL HWCDCSerial
  #else
    #define YO_REAL_SERIAL USBSerial
  #endif
#else
  #define YO_REAL_SERIAL Serial0
#endif

class YoTee : public Stream {
  public:
    void   begin(unsigned long baud);
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* b, size_t n) override;
    int    available() override;
    int    read() override;
    int    peek() override;
    void   flush() override;
    int    availableForWrite() override;
    operator bool() const;
    using Print::write;
};
extern YoTee yoTee;

void yoNetConBegin();                  /* задача сервера (з setup, після Serial.begin) */
void yoNetConInfo(Print& o);           /* стан каналу — для команди «netcon» */
bool yoNetConPeer(char* ip, size_t cap);   /* чи під'єднаний клієнт; його адреса */
/*  Знімок екрана без паузи звуку («scrq»): кадр копіюється сюди, а рядки шле задача консолі —
    просто клієнтові, повз журнал. false — немає клієнта чи пам'яті (тоді звичайний «scr»).  */
bool yoNetConSnap(const uint16_t* fb, uint16_t w, uint16_t h);

#ifndef YO_NETCON_IMPL
  #undef Serial
  #define Serial yoTee
#endif

#endif
