#ifndef netserver_h
#define netserver_h
#include "../AsyncWebServer/ESPAsyncWebServer.h"
#define APPEND_GROUP(name) strcat(nsBuf, "\"" name "\",")

enum requestType_e : uint8_t  { PLAYLIST=1, STATION=2, STATIONNAME=3, ITEM=4, TITLE=5, VOLUME=6, NRSSI=7, BITRATE=8, MODE=9, EQUALIZER=10, BALANCE=11, PLAYLISTSAVED=12, STARTUP=13, GETINDEX=14, GETACTIVE=15, GETSYSTEM=16, GETSCREEN=17, GETTIMEZONE=18, GETWEATHER=19, GETCONTROLS=20, DSPON=21, SDPOS=22, SDLEN=23, SDSNUFFLE=24, SDINIT=25, GETPLAYERMODE=26, CHANGEMODE=27 };
enum import_e      : uint8_t  { IMDONE=0, IMPL=1, IMWIFI=2 };
const char emptyfs_html[] PROGMEM = R"~(
<!DOCTYPE html><html lang="uk"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1"><meta name="theme-color" content="#161b25"><link rel="icon" type="image/png" href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAMAAACdt4HsAAAB/lBMVEUWGyTq1lslJyQAAADizlg0NSgdICNIRi5YVTLIt1EVGiRsZjikmEgVGiTZxlazpUyRh0IVHCT44l4LER8AAD8WGyV8dDxhXDWdkka/sE/96GAIDh4AAFWBeT6GfUBAPywAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABG5d4vAAAAgHRSTlP+//8A////////yv//TP///yz//wSR////////A////wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACgtW0sAAAKNSURBVHja3ZfteuogDIADUqCf1Ha2U6e7/7tcEqhTZ3qmnPPn8Ki1JXlJAkkBNktrdxX8slW79qIGi/qvtROjvQK8bbZPqjNii4oMeNu08FJriUAWbOHFto0WvKzPBARUrwMqArSQ0doNfFQ5gOoDsgxAE2CXB9hBlQf4oT7W7iiLH309rvLs6AclE45ODX60a4TCKGW81Fsr7C3WDICAEkMj9TcD8gNYWb9WKFFqSUCXyFe1TCABpRxoeBgqDY76xQHYAKN60FpPP4T0GR9DTxKSCRQBo7oC5c6HcEfQ4XBGctGhiBiFgjzsdaEdXsuCmfxjwZX4yGFXT1Eq1kJIUg0aOtQ4ICzfmuLfMFsMo4WGB05CgfR86Lrg6V9I8IJMaR77YLkPR5misLaT4jZZHaFTtE6Vj2Ogu2sZvHpSoo/Hm2t6p8VlrGqU2SdZNpvdSdQ9XleWM6+SObnZ8zU1CkyfAjTHtfZPAOiCYRdCcuF9ceE9uRDYBSO5wEE8JUWeThMNMMv0EegkB9EuwaM4qdriNHT0r8NJsPHZEszHywCWkYuO/Y4TGdU4TzFNkiXCSqzTnFvKucMEvHzoiYbTgfLUprUhpSNHkZKJTGgwey8AjZazATwZUlWL6Ww+ccB56PFe2wiwlFH9MJNJZiWdLcxLQQGe6AWApQg1OKe4oMxSPbCwTyXNpvsLgG9TSduLNdGCvymqF4C9Kap+rSxTKTk0EiAMRiwG36sRo32bHt8AR716/WXpUH+UACPOr/vT69Zf6d8DYHR+fOrlfQ94uv0HgM8IeL0VfwXg8gAmF6CGHIB22PSTO8VnN5rZW93szXb2dj/7wJF/5Mk/dGUf+/IPnrlH3/zDd9bx/ws6tRsG9Kz+jwAAAABJRU5ErkJggg=="><title>Файли сторінки · ПОТУЖНЕ РАДІО</title><style>*{box-sizing:border-box}html,body{margin:0;background:#000;color:#f2f2f2;font:15px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}.top{display:flex;align-items:center;gap:10px;height:56px;padding:0 20px;border-bottom:1px solid #262626}.top b{color:#e6d25a;letter-spacing:.08em;font-size:15px}.top small{color:#8c8c8c;margin-left:auto;font-size:13px}.page{max-width:620px;margin:0 auto;padding:20px}.card{background:#111;border:1px solid #262626;border-radius:12px;padding:16px;margin-bottom:14px}.card h2{font-size:13px;font-weight:650;letter-spacing:.08em;text-transform:uppercase;color:#8c8c8c;margin:0 0 12px}p{margin:0 0 10px}.note{color:#8c8c8c;font-size:13px}a{color:#e6d25a;text-decoration:none}a:hover{text-decoration:underline}.btn{display:inline-flex;align-items:center;justify-content:center;height:38px;padding:0 15px;border-radius:8px;border:1px solid #333;background:#1b1b1b;color:#f2f2f2;font:inherit;font-weight:550;cursor:pointer;margin:0}.btn.acc{background:#e6d25a;border-color:#e6d25a;color:#000}.bar{display:flex;flex-wrap:wrap;gap:10px;align-items:center;margin:10px 0}input[type=text],input[type=password]{width:100%;height:40px;background:#0b0b0b;border:1px solid #333;border-radius:8px;padding:0 12px;color:#f2f2f2;font:inherit;outline:none;margin-bottom:10px}input:focus{border-color:#e6d25a}label.f{display:block;color:#8c8c8c;font-size:13px;margin-bottom:5px}.hidden{display:none}code{font:13px ui-monospace,Menlo,monospace;background:#1c1c1c;border:1px solid #333;border-radius:5px;padding:1px 5px}</style>
<script type="text/javascript" src="/variables.js"></script>
</head><body>
<header class="top"><svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="#e6d25a" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="11" r="2"/><path d="M8.5 7.5a5 5 0 0 0 0 7M15.5 7.5a5 5 0 0 1 0 7M5.6 4.6a9 9 0 0 0 0 12.8M18.4 4.6a9 9 0 0 1 0 12.8M12 13v8"/></svg><b>ПОТУЖНЕ РАДІО</b><small id="version"></small></header>
<main class="page">
<form action="/webboard" method="post" enctype="multipart/form-data">
<section class="card"><h2>Файли сторінки</h2>
<p>Веб-сторінки на радіо немає або вона пошкоджена. Виберіть файли <b>app.js.gz</b> і <b>app.css.gz</b> з теки проєкту <code>firmware/web/</code> і надішліть їх.</p>
<div class="bar"><label class="btn">Вибрати файли<input type="file" name="www" multiple accept=".gz,.js,.css" hidden onchange="this.parentNode.nextElementSibling.textContent=[].map.call(this.files,function(f){return f.name}).join(', ')||'файл не вибрано'"></label><span class="note">файл не вибрано</span></div>
</section>
<section class="card"><h2>Резервна копія</h2>
<p class="note">Необов'язково: разом можна повернути список станцій <b>playlist.csv</b> і мережі <b>wifi.csv</b> із резервної копії.</p>
<div class="bar"><label class="btn">Вибрати списки<input type="file" name="data" multiple accept=".csv" hidden onchange="this.parentNode.nextElementSibling.textContent=[].map.call(this.files,function(f){return f.name}).join(', ')||'файл не вибрано'"></label><span class="note">файл не вибрано</span></div>
</section>
<div class="bar"><button class="btn acc" type="submit">Надіслати файли</button></div>
</form>
<form name="wifiform" method="post" enctype="multipart/form-data" id="wupload">
<section class="card"><h2>Wi-Fi</h2>
<p class="note">Сторінка радіо доступна, лише коли воно в мережі. Мережу виберіть на самому радіо: торкніться екрана й візьміть її зі списку.</p>
<label class="f" for="ssid">Назва мережі</label><input type="text" id="ssid" name="ssid" value="" maxlength="30" autocomplete="off">
<label class="f" for="pass">Пароль</label><input type="password" id="pass" name="pass" value="" maxlength="40" autocomplete="off">
<div class="bar"><button class="btn" type="submit">Зберегти мережу</button></div>
</section>
</form>
<p class="note"><a href="/">На головну</a> · <a href="/emergency">Аварійне оновлення прошивки</a></p>
</main>
</body>
<script>
document.wifiform.action = `/${formAction}`;
if(playMode=='player') document.getElementById("wupload").classList.add("hidden");
document.getElementById("version").textContent='версія '+yoVersion;
</script>
</html>
)~";
const char index_html[] PROGMEM = R"~(
<!DOCTYPE html>
<html lang="uk">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#161b25">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black">
<link rel="icon" type="image/png" href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAMAAACdt4HsAAAB/lBMVEUWGyTq1lslJyQAAADizlg0NSgdICNIRi5YVTLIt1EVGiRsZjikmEgVGiTZxlazpUyRh0IVHCT44l4LER8AAD8WGyV8dDxhXDWdkka/sE/96GAIDh4AAFWBeT6GfUBAPywAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABG5d4vAAAAgHRSTlP+//8A////////yv//TP///yz//wSR////////A////wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACgtW0sAAAKNSURBVHja3ZfteuogDIADUqCf1Ha2U6e7/7tcEqhTZ3qmnPPn8Ki1JXlJAkkBNktrdxX8slW79qIGi/qvtROjvQK8bbZPqjNii4oMeNu08FJriUAWbOHFto0WvKzPBARUrwMqArSQ0doNfFQ5gOoDsgxAE2CXB9hBlQf4oT7W7iiLH309rvLs6AclE45ODX60a4TCKGW81Fsr7C3WDICAEkMj9TcD8gNYWb9WKFFqSUCXyFe1TCABpRxoeBgqDY76xQHYAKN60FpPP4T0GR9DTxKSCRQBo7oC5c6HcEfQ4XBGctGhiBiFgjzsdaEdXsuCmfxjwZX4yGFXT1Eq1kJIUg0aOtQ4ICzfmuLfMFsMo4WGB05CgfR86Lrg6V9I8IJMaR77YLkPR5misLaT4jZZHaFTtE6Vj2Ogu2sZvHpSoo/Hm2t6p8VlrGqU2SdZNpvdSdQ9XleWM6+SObnZ8zU1CkyfAjTHtfZPAOiCYRdCcuF9ceE9uRDYBSO5wEE8JUWeThMNMMv0EegkB9EuwaM4qdriNHT0r8NJsPHZEszHywCWkYuO/Y4TGdU4TzFNkiXCSqzTnFvKucMEvHzoiYbTgfLUprUhpSNHkZKJTGgwey8AjZazATwZUlWL6Ww+ccB56PFe2wiwlFH9MJNJZiWdLcxLQQGe6AWApQg1OKe4oMxSPbCwTyXNpvsLgG9TSduLNdGCvymqF4C9Kap+rSxTKTk0EiAMRiwG36sRo32bHt8AR716/WXpUH+UACPOr/vT69Zf6d8DYHR+fOrlfQ94uv0HgM8IeL0VfwXg8gAmF6CGHIB22PSTO8VnN5rZW93szXb2dj/7wJF/5Mk/dGUf+/IPnrlH3/zDd9bx/ws6tRsG9Kz+jwAAAABJRU5ErkJggg==">
<link rel="apple-touch-icon" href="/apple-touch-icon.png">
<title>ПОТУЖНЕ РАДІО</title>
<link rel="stylesheet" href="app.css" onerror="this.onerror=null;var l=this;setTimeout(function(){l.href='app.css?r=1'},900)">
<script src="variables.js"></script>
<style>html,body{margin:0;background:#000}.boot{display:flex;align-items:center;justify-content:center;flex-direction:column;gap:14px;height:100vh;color:#e6d25a;font:600 15px system-ui,sans-serif;letter-spacing:.1em;text-align:center}.boot a{color:#f2f2f2;letter-spacing:0;font-weight:500}</style>
</head>
<body>
<div id="app"><div class="boot">ПОТУЖНЕ РАДІО</div></div>
<script>
/* Плата інколи не встигає віддати файл із першого разу, тож пробуємо ще. */
(function load(n){var s=document.createElement('script');s.src='app.js'+(n?'?r='+n:'');
s.onerror=function(){s.remove();if(n<4)setTimeout(function(){load(n+1)},800*(n+1));
else document.getElementById('app').innerHTML='<div class="boot">Сторінка не завантажилась<a href="/">Спробувати ще раз</a><a href="/webboard?raw=1">Залити файли сторінки</a></div>';};
document.body.appendChild(s);})(0);
</script>
</body>
</html>
)~";
const char emergency_form[] PROGMEM = R"~(
<!DOCTYPE html><html lang="uk"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1"><meta name="theme-color" content="#161b25"><link rel="icon" type="image/png" href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAMAAACdt4HsAAAB/lBMVEUWGyTq1lslJyQAAADizlg0NSgdICNIRi5YVTLIt1EVGiRsZjikmEgVGiTZxlazpUyRh0IVHCT44l4LER8AAD8WGyV8dDxhXDWdkka/sE/96GAIDh4AAFWBeT6GfUBAPywAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABG5d4vAAAAgHRSTlP+//8A////////yv//TP///yz//wSR////////A////wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACgtW0sAAAKNSURBVHja3ZfteuogDIADUqCf1Ha2U6e7/7tcEqhTZ3qmnPPn8Ki1JXlJAkkBNktrdxX8slW79qIGi/qvtROjvQK8bbZPqjNii4oMeNu08FJriUAWbOHFto0WvKzPBARUrwMqArSQ0doNfFQ5gOoDsgxAE2CXB9hBlQf4oT7W7iiLH309rvLs6AclE45ODX60a4TCKGW81Fsr7C3WDICAEkMj9TcD8gNYWb9WKFFqSUCXyFe1TCABpRxoeBgqDY76xQHYAKN60FpPP4T0GR9DTxKSCRQBo7oC5c6HcEfQ4XBGctGhiBiFgjzsdaEdXsuCmfxjwZX4yGFXT1Eq1kJIUg0aOtQ4ICzfmuLfMFsMo4WGB05CgfR86Lrg6V9I8IJMaR77YLkPR5misLaT4jZZHaFTtE6Vj2Ogu2sZvHpSoo/Hm2t6p8VlrGqU2SdZNpvdSdQ9XleWM6+SObnZ8zU1CkyfAjTHtfZPAOiCYRdCcuF9ceE9uRDYBSO5wEE8JUWeThMNMMv0EegkB9EuwaM4qdriNHT0r8NJsPHZEszHywCWkYuO/Y4TGdU4TzFNkiXCSqzTnFvKucMEvHzoiYbTgfLUprUhpSNHkZKJTGgwey8AjZazATwZUlWL6Ww+ccB56PFe2wiwlFH9MJNJZiWdLcxLQQGe6AWApQg1OKe4oMxSPbCwTyXNpvsLgG9TSduLNdGCvymqF4C9Kap+rSxTKTk0EiAMRiwG36sRo32bHt8AR716/WXpUH+UACPOr/vT69Zf6d8DYHR+fOrlfQ94uv0HgM8IeL0VfwXg8gAmF6CGHIB22PSTO8VnN5rZW93szXb2dj/7wJF/5Mk/dGUf+/IPnrlH3/zDd9bx/ws6tRsG9Kz+jwAAAABJRU5ErkJggg=="><title>Аварійне оновлення · ПОТУЖНЕ РАДІО</title><style>*{box-sizing:border-box}html,body{margin:0;background:#000;color:#f2f2f2;font:15px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}.top{display:flex;align-items:center;gap:10px;height:56px;padding:0 20px;border-bottom:1px solid #262626}.top b{color:#e6d25a;letter-spacing:.08em;font-size:15px}.top small{color:#8c8c8c;margin-left:auto;font-size:13px}.page{max-width:620px;margin:0 auto;padding:20px}.card{background:#111;border:1px solid #262626;border-radius:12px;padding:16px;margin-bottom:14px}.card h2{font-size:13px;font-weight:650;letter-spacing:.08em;text-transform:uppercase;color:#8c8c8c;margin:0 0 12px}p{margin:0 0 10px}.note{color:#8c8c8c;font-size:13px}a{color:#e6d25a;text-decoration:none}a:hover{text-decoration:underline}.btn{display:inline-flex;align-items:center;justify-content:center;height:38px;padding:0 15px;border-radius:8px;border:1px solid #333;background:#1b1b1b;color:#f2f2f2;font:inherit;font-weight:550;cursor:pointer;margin:0}.btn.acc{background:#e6d25a;border-color:#e6d25a;color:#000}.bar{display:flex;flex-wrap:wrap;gap:10px;align-items:center;margin:10px 0}input[type=text],input[type=password]{width:100%;height:40px;background:#0b0b0b;border:1px solid #333;border-radius:8px;padding:0 12px;color:#f2f2f2;font:inherit;outline:none;margin-bottom:10px}input:focus{border-color:#e6d25a}label.f{display:block;color:#8c8c8c;font-size:13px;margin-bottom:5px}.hidden{display:none}code{font:13px ui-monospace,Menlo,monospace;background:#1c1c1c;border:1px solid #333;border-radius:5px;padding:1px 5px}</style>
</head><body>
<header class="top"><svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="#e6d25a" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="11" r="2"/><path d="M8.5 7.5a5 5 0 0 0 0 7M15.5 7.5a5 5 0 0 1 0 7M5.6 4.6a9 9 0 0 0 0 12.8M18.4 4.6a9 9 0 0 1 0 12.8M12 13v8"/></svg><b>ПОТУЖНЕ РАДІО</b></header>
<main class="page">
<form method="POST" action="/update" enctype="multipart/form-data">
<section class="card"><h2>Аварійне оновлення прошивки</h2>
<p>Файл <b>firmware/PotuzhneRadio-ES3C28P-update.bin</b> із теки проєкту. Повний образ <b>PotuzhneRadio-ES3C28P-full.bin</b> сюди не підходить: він для кабелю.</p>
<input type="hidden" name="updatetarget" value="fw" />
<div class="bar"><label class="btn">Вибрати прошивку<input type="file" name="update" accept=".bin" hidden onchange="this.parentNode.nextElementSibling.textContent=[].map.call(this.files,function(f){return f.name}).join(', ')||'файл не вибрано'"></label><span class="note">файл не вибрано</span></div>
<div class="bar"><button class="btn acc" type="submit">Оновити</button></div>
<p class="note">Після «OK» радіо перезавантажиться саме; станції й мережі лишаються.</p>
</section>
<p class="note"><a href="/">На головну</a></p>
</main></body></html>
)~";
struct nsRequestParams_t
{
  requestType_e type;
  uint32_t clientId;     /* номер клієнта веб-сокета росте з кожним підключенням — не uint8_t */
};

class NetServer {
  public:
    import_e importRequest;
    bool resumePlay;
    char chunkedPathBuffer[40];
    char nsBuf[BUFLEN], nsBuf2[BUFLEN];
  public:
    NetServer() {};
    bool begin(bool quiet=false);
    void loop();
    void requestOnChange(requestType_e request, uint32_t clientId);
    void setRSSI(int val) { rssi = val; };
    int  getRSSI()        { return rssi; };
    void chunkedHtmlPage(const String& contentType, AsyncWebServerRequest *request, const char * path);
    void onWsMessage(void *arg, uint8_t *data, size_t len, uint32_t clientId);
    bool irRecordEnable;
#if IR_PIN!=255
    void irToWs(const char* protocol, uint64_t irvalue);
    void irValsToWs(); 
#endif
    void resetQueue();
  private:
    requestType_e request;
    QueueHandle_t nsQueue;
    char _wscmd[65], _wsval[65];
    char wsBuf[BUFLEN*2];
    int rssi;
    uint32_t playerBufMax;
    void getPlaylist(uint32_t clientId);
    bool importPlaylist();
    static size_t chunkedHtmlPageCallback(uint8_t* buffer, size_t maxLen, size_t index);
    void processQueue();
    int _readPlaylistLine(File &file, char * line, size_t size);
};

extern NetServer netserver;
extern AsyncWebSocket websocket;

#endif
