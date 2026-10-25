/*  ES3C35P (ST77922, ландшафт 480×320): те кілька старих сталих yoRadio, що
    ще вживаються (заставка-логотип при старті). Решту екрана малює src/m2.  */
#ifndef displayST77922conf_h
#define displayST77922conf_h

#define DSP_WIDTH       480
#define TFT_FRAMEWDT    8
#define MAX_WIDTH       DSP_WIDTH-TFT_FRAMEWDT*2
/*  Логотип 240×76 — посередині висоти; верх кратний 8 (вимога ST77922 до рядків).  */
#define bootLogoTop     120

#endif
