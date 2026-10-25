#include "yoDlna.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include "../core/options.h"
#include "../core/config.h"
#include "../core/player.h"
#include "../core/display.h"
#include "../core/netserver.h"
#include "yoExtras.h"
#include "yoVersion.h"

YoDlna dlna;

namespace {

const uint16_t   DLNA_PORT = 8080;
const IPAddress  SSDP_IP(239, 255, 255, 250);
const uint16_t   SSDP_PORT = 1900;

WiFiUDP    udp;
WiFiServer srv(DLNA_PORT);

/*  Опис пристрою й служб. Керівники (телефон, VLC, Windows) читають їх один раз
    і далі шлють команди на /ctl/*.  */
const char DESC[] = R"(<?xml version="1.0" encoding="utf-8"?>
<root xmlns="urn:schemas-upnp-org:device-1-0"><specVersion><major>1</major><minor>0</minor></specVersion>
<device><deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>
<friendlyName>%s</friendlyName><manufacturer>ПОТУЖНЕ РАДІО</manufacturer>
<manufacturerURL>https://github.com/roman885-85/potuzhne-radio</manufacturerURL>
<modelDescription>Інтернет-радіо )" PR_BOARD R"(</modelDescription><modelName>ПОТУЖНЕ РАДІО</modelName>
<modelNumber>%s</modelNumber><serialNumber>%s</serialNumber><UDN>uuid:%s</UDN>
<serviceList>
<service><serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType><serviceId>urn:upnp-org:serviceId:AVTransport</serviceId><SCPDURL>/avt.xml</SCPDURL><controlURL>/ctl/avt</controlURL><eventSubURL>/evt/avt</eventSubURL></service>
<service><serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType><serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId><SCPDURL>/rc.xml</SCPDURL><controlURL>/ctl/rc</controlURL><eventSubURL>/evt/rc</eventSubURL></service>
<service><serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType><serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId><SCPDURL>/cm.xml</SCPDURL><controlURL>/ctl/cm</controlURL><eventSubURL>/evt/cm</eventSubURL></service>
</serviceList></device></root>)";

/*  Списки дій: без них деякі керівники не показують пристрій узагалі.  */
const char SCPD_AVT[] = R"(<?xml version="1.0" encoding="utf-8"?>
<scpd xmlns="urn:schemas-upnp-org:service-1-0"><specVersion><major>1</major><minor>0</minor></specVersion><actionList>
<action><name>SetAVTransportURI</name><argumentList><argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument><argument><name>CurrentURI</name><direction>in</direction><relatedStateVariable>AVTransportURI</relatedStateVariable></argument><argument><name>CurrentURIMetaData</name><direction>in</direction><relatedStateVariable>AVTransportURIMetaData</relatedStateVariable></argument></argumentList></action>
<action><name>SetNextAVTransportURI</name><argumentList><argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument><argument><name>NextURI</name><direction>in</direction><relatedStateVariable>NextAVTransportURI</relatedStateVariable></argument><argument><name>NextURIMetaData</name><direction>in</direction><relatedStateVariable>NextAVTransportURIMetaData</relatedStateVariable></argument></argumentList></action>
<action><name>GetMediaInfo</name><argumentList><argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument><argument><name>NrTracks</name><direction>out</direction><relatedStateVariable>NumberOfTracks</relatedStateVariable></argument><argument><name>MediaDuration</name><direction>out</direction><relatedStateVariable>CurrentMediaDuration</relatedStateVariable></argument><argument><name>CurrentURI</name><direction>out</direction><relatedStateVariable>AVTransportURI</relatedStateVariable></argument><argument><name>CurrentURIMetaData</name><direction>out</direction><relatedStateVariable>AVTransportURIMetaData</relatedStateVariable></argument><argument><name>NextURI</name><direction>out</direction><relatedStateVariable>NextAVTransportURI</relatedStateVariable></argument><argument><name>NextURIMetaData</name><direction>out</direction><relatedStateVariable>NextAVTransportURIMetaData</relatedStateVariable></argument><argument><name>PlayMedium</name><direction>out</direction><relatedStateVariable>PlaybackStorageMedium</relatedStateVariable></argument><argument><name>RecordMedium</name><direction>out</direction><relatedStateVariable>RecordStorageMedium</relatedStateVariable></argument><argument><name>WriteStatus</name><direction>out</direction><relatedStateVariable>RecordMediumWriteStatus</relatedStateVariable></argument></argumentList></action>
<action><name>GetTransportInfo</name><argumentList><argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument><argument><name>CurrentTransportState</name><direction>out</direction><relatedStateVariable>TransportState</relatedStateVariable></argument><argument><name>CurrentTransportStatus</name><direction>out</direction><relatedStateVariable>TransportStatus</relatedStateVariable></argument><argument><name>CurrentSpeed</name><direction>out</direction><relatedStateVariable>TransportPlaySpeed</relatedStateVariable></argument></argumentList></action>
<action><name>GetPositionInfo</name><argumentList><argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument><argument><name>Track</name><direction>out</direction><relatedStateVariable>CurrentTrack</relatedStateVariable></argument><argument><name>TrackDuration</name><direction>out</direction><relatedStateVariable>CurrentTrackDuration</relatedStateVariable></argument><argument><name>TrackMetaData</name><direction>out</direction><relatedStateVariable>CurrentTrackMetaData</relatedStateVariable></argument><argument><name>TrackURI</name><direction>out</direction><relatedStateVariable>CurrentTrackURI</relatedStateVariable></argument><argument><name>RelTime</name><direction>out</direction><relatedStateVariable>RelativeTimePosition</relatedStateVariable></argument><argument><name>AbsTime</name><direction>out</direction><relatedStateVariable>AbsoluteTimePosition</relatedStateVariable></argument><argument><name>RelCount</name><direction>out</direction><relatedStateVariable>RelativeCounterPosition</relatedStateVariable></argument><argument><name>AbsCount</name><direction>out</direction><relatedStateVariable>AbsoluteCounterPosition</relatedStateVariable></argument></argumentList></action>
<action><name>GetDeviceCapabilities</name><argumentList><argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument><argument><name>PlayMedia</name><direction>out</direction><relatedStateVariable>PossiblePlaybackStorageMedia</relatedStateVariable></argument><argument><name>RecMedia</name><direction>out</direction><relatedStateVariable>PossibleRecordStorageMedia</relatedStateVariable></argument><argument><name>RecQualityModes</name><direction>out</direction><relatedStateVariable>PossibleRecordQualityModes</relatedStateVariable></argument></argumentList></action>
<action><name>GetTransportSettings</name><argumentList><argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument><argument><name>PlayMode</name><direction>out</direction><relatedStateVariable>CurrentPlayMode</relatedStateVariable></argument><argument><name>RecQualityMode</name><direction>out</direction><relatedStateVariable>CurrentRecordQualityMode</relatedStateVariable></argument></argumentList></action>
<action><name>Stop</name><argumentList><argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument></argumentList></action>
<action><name>Play</name><argumentList><argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument><argument><name>Speed</name><direction>in</direction><relatedStateVariable>TransportPlaySpeed</relatedStateVariable></argument></argumentList></action>
<action><name>Pause</name><argumentList><argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument></argumentList></action>
<action><name>Seek</name><argumentList><argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument><argument><name>Unit</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_SeekMode</relatedStateVariable></argument><argument><name>Target</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_SeekTarget</relatedStateVariable></argument></argumentList></action>
</actionList><serviceStateTable>
<stateVariable sendEvents="yes"><name>LastChange</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>TransportState</name><dataType>string</dataType><allowedValueList><allowedValue>STOPPED</allowedValue><allowedValue>PLAYING</allowedValue><allowedValue>PAUSED_PLAYBACK</allowedValue><allowedValue>TRANSITIONING</allowedValue></allowedValueList></stateVariable>
<stateVariable sendEvents="no"><name>TransportStatus</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>TransportPlaySpeed</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>NumberOfTracks</name><dataType>ui4</dataType></stateVariable>
<stateVariable sendEvents="no"><name>CurrentTrack</name><dataType>ui4</dataType></stateVariable>
<stateVariable sendEvents="no"><name>CurrentTrackDuration</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>CurrentMediaDuration</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>CurrentTrackMetaData</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>CurrentTrackURI</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>AVTransportURI</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>AVTransportURIMetaData</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>NextAVTransportURI</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>NextAVTransportURIMetaData</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>RelativeTimePosition</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>AbsoluteTimePosition</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>RelativeCounterPosition</name><dataType>i4</dataType></stateVariable>
<stateVariable sendEvents="no"><name>AbsoluteCounterPosition</name><dataType>i4</dataType></stateVariable>
<stateVariable sendEvents="no"><name>PlaybackStorageMedium</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>RecordStorageMedium</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>PossiblePlaybackStorageMedia</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>PossibleRecordStorageMedia</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>RecordMediumWriteStatus</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>CurrentPlayMode</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>CurrentRecordQualityMode</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>PossibleRecordQualityModes</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_InstanceID</name><dataType>ui4</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_SeekMode</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_SeekTarget</name><dataType>string</dataType></stateVariable>
</serviceStateTable></scpd>)";

const char SCPD_RC[] = R"(<?xml version="1.0" encoding="utf-8"?>
<scpd xmlns="urn:schemas-upnp-org:service-1-0"><specVersion><major>1</major><minor>0</minor></specVersion><actionList>
<action><name>GetVolume</name><argumentList><argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument><argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument><argument><name>CurrentVolume</name><direction>out</direction><relatedStateVariable>Volume</relatedStateVariable></argument></argumentList></action>
<action><name>SetVolume</name><argumentList><argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument><argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument><argument><name>DesiredVolume</name><direction>in</direction><relatedStateVariable>Volume</relatedStateVariable></argument></argumentList></action>
<action><name>GetMute</name><argumentList><argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument><argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument><argument><name>CurrentMute</name><direction>out</direction><relatedStateVariable>Mute</relatedStateVariable></argument></argumentList></action>
<action><name>SetMute</name><argumentList><argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument><argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument><argument><name>DesiredMute</name><direction>in</direction><relatedStateVariable>Mute</relatedStateVariable></argument></argumentList></action>
<action><name>ListPresets</name><argumentList><argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument><argument><name>CurrentPresetNameList</name><direction>out</direction><relatedStateVariable>PresetNameList</relatedStateVariable></argument></argumentList></action>
</actionList><serviceStateTable>
<stateVariable sendEvents="yes"><name>LastChange</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>Volume</name><dataType>ui2</dataType><allowedValueRange><minimum>0</minimum><maximum>100</maximum><step>1</step></allowedValueRange></stateVariable>
<stateVariable sendEvents="no"><name>Mute</name><dataType>boolean</dataType></stateVariable>
<stateVariable sendEvents="no"><name>PresetNameList</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_InstanceID</name><dataType>ui4</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_Channel</name><dataType>string</dataType></stateVariable>
</serviceStateTable></scpd>)";

const char SCPD_CM[] = R"(<?xml version="1.0" encoding="utf-8"?>
<scpd xmlns="urn:schemas-upnp-org:service-1-0"><specVersion><major>1</major><minor>0</minor></specVersion><actionList>
<action><name>GetProtocolInfo</name><argumentList><argument><name>Source</name><direction>out</direction><relatedStateVariable>SourceProtocolInfo</relatedStateVariable></argument><argument><name>Sink</name><direction>out</direction><relatedStateVariable>SinkProtocolInfo</relatedStateVariable></argument></argumentList></action>
<action><name>GetCurrentConnectionIDs</name><argumentList><argument><name>ConnectionIDs</name><direction>out</direction><relatedStateVariable>CurrentConnectionIDs</relatedStateVariable></argument></argumentList></action>
<action><name>GetCurrentConnectionInfo</name><argumentList><argument><name>ConnectionID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable></argument><argument><name>RcsID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_RcsID</relatedStateVariable></argument><argument><name>AVTransportID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_AVTransportID</relatedStateVariable></argument><argument><name>ProtocolInfo</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ProtocolInfo</relatedStateVariable></argument><argument><name>PeerConnectionManager</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionManager</relatedStateVariable></argument><argument><name>PeerConnectionID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable></argument><argument><name>Direction</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Direction</relatedStateVariable></argument><argument><name>Status</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionStatus</relatedStateVariable></argument></argumentList></action>
</actionList><serviceStateTable>
<stateVariable sendEvents="yes"><name>SourceProtocolInfo</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="yes"><name>SinkProtocolInfo</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="yes"><name>CurrentConnectionIDs</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_ConnectionStatus</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_ConnectionManager</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_Direction</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_ProtocolInfo</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_ConnectionID</name><dataType>i4</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_AVTransportID</name><dataType>i4</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_RcsID</name><dataType>i4</dataType></stateVariable>
</serviceStateTable></scpd>)";

/*  Що радіо вміє програти: цим списком керівники вирішують, чи показувати
    пристрій для конкретного файла.  */
const char SINK[] =
  "http-get:*:audio/mpeg:*,http-get:*:audio/mp3:*,http-get:*:audio/x-mpeg:*,"
  "http-get:*:audio/mp4:*,http-get:*:audio/x-m4a:*,http-get:*:audio/aac:*,http-get:*:audio/aacp:*,"
  "http-get:*:audio/flac:*,http-get:*:audio/x-flac:*,http-get:*:audio/wav:*,http-get:*:audio/x-wav:*,"
  "http-get:*:audio/wave:*,http-get:*:audio/vnd.wave:*,http-get:*:audio/L16:*,http-get:*:application/ogg:*";

/*  Великі буфери — у PSRAM, а не на стеку задачі: стек тут спільний із
    мережею (lwIP + Wi-Fi ідуть просто з цієї задачі), і масиви на кілька
    кілобайтів валили її разом із радіо.  */
char* bigBuf = nullptr;                 /* тіло запиту (DIDL буває довгий) */
char* outBuf = nullptr;                 /* відповідь */
char* hdrBuf = nullptr;                 /* заголовки запиту */
const size_t BIG = 8192, OUT = 6144, HDR = 1536;

/*  Значення тега без урахування простору імен: <dc:title>…</dc:title>.  */
bool tagValue(const char* body, const char* tag, char* out, size_t cap){
  if(!body || !out || !cap) return false;
  out[0] = 0;
  char open[48];
  snprintf(open, sizeof(open), "<%s", tag);
  const char* p = body;
  while((p = strstr(p, open))){
    const char* q = p + strlen(open);
    if(*q != '>' && *q != ' ' && *q != ':'){ p = q; continue; }   /* інший тег, що починається так само */
    q = strchr(q, '>');
    if(!q) return false;
    q++;
    char close[48];
    snprintf(close, sizeof(close), "</%s", tag);
    const char* e = strstr(q, close);
    if(!e){
      /*  тег із простором імен у закритті: шукаємо будь-яке «</…tag>»  */
      e = strstr(q, "</");
      if(!e) return false;
    }
    size_t n = (size_t)(e - q); if(n >= cap) n = cap - 1;
    memcpy(out, q, n); out[n] = 0;
    return n > 0;
  }
  return false;
}

/*  &amp; &lt; &gt; &quot; &#39; — у звичайні знаки (посилання й назви приходять саме так).  */
void unescape(char* s){
  char* w = s;
  for(const char* r = s; *r; ){
    if(*r == '&'){
      if(!strncmp(r, "&amp;", 5)){ *w++ = '&'; r += 5; continue; }
      if(!strncmp(r, "&lt;", 4)){ *w++ = '<'; r += 4; continue; }
      if(!strncmp(r, "&gt;", 4)){ *w++ = '>'; r += 4; continue; }
      if(!strncmp(r, "&quot;", 6)){ *w++ = '"'; r += 6; continue; }
      if(!strncmp(r, "&apos;", 6)){ *w++ = '\''; r += 6; continue; }
      if(!strncmp(r, "&#39;", 5)){ *w++ = '\''; r += 5; continue; }
    }
    *w++ = *r++;
  }
  *w = 0;
}

void escape(const char* s, char* out, size_t cap){
  size_t n = 0;
  for(; *s && n + 7 < cap; s++){
    switch(*s){
      case '&': memcpy(out + n, "&amp;", 5); n += 5; break;
      case '<': memcpy(out + n, "&lt;", 4); n += 4; break;
      case '>': memcpy(out + n, "&gt;", 4); n += 4; break;
      case '"': memcpy(out + n, "&quot;", 6); n += 6; break;
      default: out[n++] = *s;
    }
  }
  out[n] = 0;
}

void sendHead(WiFiClient& c, int code, const char* ctype, size_t len, const char* extra = nullptr){
  c.printf("HTTP/1.1 %d %s\r\n", code, code == 200 ? "OK" : (code == 404 ? "Not Found" : (code == 500 ? "Internal Server Error" : "Bad Request")));
  c.printf("Content-Type: %s\r\n", ctype);
  c.printf("Content-Length: %u\r\n", (unsigned)len);
  c.print("Server: ESP32 UPnP/1.0 PotuzhneRadio\r\n");
  c.print("Connection: close\r\n");
  if(extra) c.print(extra);
  c.print("\r\n");
}

void sendBody(WiFiClient& c, int code, const char* ctype, const char* body, const char* extra = nullptr){
  sendHead(c, code, ctype, strlen(body), extra);
  c.print(body);
}

/*  Відповідь SOAP: тіло дії всередині конверта.  */
void soapOk(WiFiClient& c, const char* svc, const char* action, const char* args){
  char* b = outBuf;
  snprintf(b, OUT,
    "<?xml version=\"1.0\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
    "<s:Body><u:%sResponse xmlns:u=\"urn:schemas-upnp-org:service:%s:1\">%s</u:%sResponse></s:Body></s:Envelope>",
    action, svc, args ? args : "", action);
  sendBody(c, 200, "text/xml; charset=\"utf-8\"", b);
}

void soapErr(WiFiClient& c, int code, const char* text){
  char b[420];
  snprintf(b, sizeof(b),
    "<?xml version=\"1.0\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
    "<s:Body><s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail>"
    "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>%d</errorCode><errorDescription>%s</errorDescription>"
    "</UPnPError></detail></s:Fault></s:Body></s:Envelope>", code, text);
  sendBody(c, 500, "text/xml; charset=\"utf-8\"", b);
}

uint8_t volTo100(){ return (uint8_t)((config.store.volume * 100 + 127) / 254); }
uint8_t volFrom100(int v){ if(v < 0) v = 0; if(v > 100) v = 100; return (uint8_t)((v * 254 + 50) / 100); }

}  // namespace

bool YoDlna::on() const { return extras.s.dlnaOn != 0; }

void YoDlna::setOn(bool v){
  if(on() == v) return;
  extras.s.dlnaOn = v ? 1 : 0;
  extras.changed();
  if(v) begin(); else stop();
}

const char* friendlyOf(){
  static char name[48];
  const char* mdns = config.store.mdnsname;
  if(mdns && mdns[0]) snprintf(name, sizeof(name), "ПОТУЖНЕ РАДІО (%s)", mdns);
  else strlcpy(name, "ПОТУЖНЕ РАДІО", sizeof(name));
  return name;
}

void yoDlnaTask(void* arg){ ((YoDlna*)arg)->_run(); }

void YoDlna::begin(){
  if(!on() || _task) return;
  if(!_uuid[0]){
    uint64_t mac = ESP.getEfuseMac();
    snprintf(_uuid, sizeof(_uuid), "4d495241-444f-1000-8000-%02x%02x%02x%02x%02x%02x",
             (unsigned)(mac & 0xFF), (unsigned)((mac >> 8) & 0xFF), (unsigned)((mac >> 16) & 0xFF),
             (unsigned)((mac >> 24) & 0xFF), (unsigned)((mac >> 32) & 0xFF), (unsigned)((mac >> 40) & 0xFF));
  }
  if(!bigBuf) bigBuf = (char*)ps_malloc(BIG);
  if(!outBuf) outBuf = (char*)ps_malloc(OUT);
  if(!hdrBuf) hdrBuf = (char*)ps_malloc(HDR);
  if(!bigBuf || !outBuf || !hdrBuf){ Serial.println("##DLNA#\tне вистачило пам'яті"); return; }
  TaskHandle_t h = nullptr;
  if(xTaskCreatePinnedToCore(yoDlnaTask, "dlna", 8192, this, 1, &h, 0) != pdPASS){
    Serial.println("##DLNA#\tзадача не створилась");
    return;
  }
  _task = h;
  Serial.printf("##DLNA#\tбездротова колонка: %s, порт %u\n", friendlyOf(), (unsigned)DLNA_PORT);
}

void YoDlna::stop(){
  if(!_task) return;
  _notify(false);
  srv.end();
  udp.stop();
  TaskHandle_t h = (TaskHandle_t)_task; _task = nullptr;
  _started = false;
  vTaskDelete(h);
  Serial.println("##DLNA#\tвимкнено");
}

void YoDlna::stopped(){ _playing = false; }

void YoDlna::_run(){
  udp.beginMulticast(SSDP_IP, SSDP_PORT);
  srv.begin();
  srv.setNoDelay(true);
  _started = true;
  vTaskDelay(pdMS_TO_TICKS(300));
  _notify(true);
  uint32_t alive = millis();
  for(;;){
    _ssdp();
    _http();
    if(millis() - alive > 600000UL){ alive = millis(); _notify(true); }   /* раз на 10 хв нагадуємо про себе */
    /*  З'єднання з файлом триває кілька секунд — раніше не вважаємо, що доріжка скінчилась.  */
    if(_playing && millis() - _startMs > 12000 && !player.isRunning() && player.status() != PLAYING) _playing = false;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

/*  ---------- SSDP: «хто тут колонка?» ---------- */

void YoDlna::_search(const char* st, const char* addrHost, uint16_t port){
  char usn[96];
  if(!strcmp(st, "upnp:rootdevice"))            snprintf(usn, sizeof(usn), "uuid:%s::upnp:rootdevice", _uuid);
  else if(!strncmp(st, "uuid:", 5))             snprintf(usn, sizeof(usn), "uuid:%s", _uuid);
  else                                          snprintf(usn, sizeof(usn), "uuid:%s::%s", _uuid, st);
  char msg[420];
  snprintf(msg, sizeof(msg),
    "HTTP/1.1 200 OK\r\n"
    "CACHE-CONTROL: max-age=1800\r\n"
    "EXT:\r\n"
    "LOCATION: http://%s:%u/desc.xml\r\n"
    "SERVER: ESP32/1.0 UPnP/1.0 PotuzhneRadio/%s\r\n"
    "ST: %s\r\n"
    "USN: %s\r\n"
    "BOOTID.UPNP.ORG: 1\r\nCONFIGID.UPNP.ORG: 1\r\n\r\n",
    WiFi.localIP().toString().c_str(), (unsigned)DLNA_PORT, prVersion(), st, usn);
  IPAddress to; to.fromString(addrHost);
  udp.beginPacket(to, port);
  udp.write((const uint8_t*)msg, strlen(msg));
  udp.endPacket();
}

void YoDlna::_ssdp(){
  int n = udp.parsePacket();
  if(n <= 0) return;
  char buf[600];
  int len = udp.read((uint8_t*)buf, sizeof(buf) - 1);
  if(len <= 0) return;
  buf[len] = 0;
  if(strncasecmp(buf, "M-SEARCH", 8) != 0) return;
  /*  ST: чого шукають  */
  const char* st = nullptr;
  for(char* p = buf; *p; p++){
    if((p == buf || p[-1] == '\n') && !strncasecmp(p, "ST:", 3)){ st = p + 3; break; }
  }
  if(!st) return;
  while(*st == ' ') st++;
  char want[80]; size_t i = 0;
  while(st[i] && st[i] != '\r' && st[i] != '\n' && i < sizeof(want) - 1){ want[i] = st[i]; i++; }
  want[i] = 0;
  const String host = udp.remoteIP().toString();
  const uint16_t port = udp.remotePort();
  const bool all = !strcmp(want, "ssdp:all");
  if(all || !strcmp(want, "upnp:rootdevice")) _search("upnp:rootdevice", host.c_str(), port);
  if(all || !strncmp(want, "uuid:", 5)){
    char u[48]; snprintf(u, sizeof(u), "uuid:%s", _uuid);
    if(all || !strcmp(want, u)) _search(u, host.c_str(), port);
  }
  if(all || !strcmp(want, "urn:schemas-upnp-org:device:MediaRenderer:1")) _search("urn:schemas-upnp-org:device:MediaRenderer:1", host.c_str(), port);
  if(all || !strcmp(want, "urn:schemas-upnp-org:service:AVTransport:1")) _search("urn:schemas-upnp-org:service:AVTransport:1", host.c_str(), port);
  if(all || !strcmp(want, "urn:schemas-upnp-org:service:RenderingControl:1")) _search("urn:schemas-upnp-org:service:RenderingControl:1", host.c_str(), port);
  if(all || !strcmp(want, "urn:schemas-upnp-org:service:ConnectionManager:1")) _search("urn:schemas-upnp-org:service:ConnectionManager:1", host.c_str(), port);
}

void YoDlna::_notify(bool alive){
  if(WiFi.status() != WL_CONNECTED) return;
  static const char* NTS[] = { "upnp:rootdevice", "urn:schemas-upnp-org:device:MediaRenderer:1",
                               "urn:schemas-upnp-org:service:AVTransport:1",
                               "urn:schemas-upnp-org:service:RenderingControl:1",
                               "urn:schemas-upnp-org:service:ConnectionManager:1", nullptr };
  char msg[460];
  for(uint8_t i = 0; NTS[i]; i++){
    char usn[96];
    if(!strcmp(NTS[i], "upnp:rootdevice")) snprintf(usn, sizeof(usn), "uuid:%s::upnp:rootdevice", _uuid);
    else snprintf(usn, sizeof(usn), "uuid:%s::%s", _uuid, NTS[i]);
    snprintf(msg, sizeof(msg),
      "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nCACHE-CONTROL: max-age=1800\r\n"
      "LOCATION: http://%s:%u/desc.xml\r\nNT: %s\r\nNTS: ssdp:%s\r\n"
      "SERVER: ESP32/1.0 UPnP/1.0 PotuzhneRadio/%s\r\nUSN: %s\r\n"
      "BOOTID.UPNP.ORG: 1\r\nCONFIGID.UPNP.ORG: 1\r\n\r\n",
      WiFi.localIP().toString().c_str(), (unsigned)DLNA_PORT, NTS[i], alive ? "alive" : "byebye", prVersion(), usn);
    udp.beginPacket(SSDP_IP, SSDP_PORT);
    udp.write((const uint8_t*)msg, strlen(msg));
    udp.endPacket();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

/*  ---------- HTTP: опис і керування ---------- */

void YoDlna::_http(){
  WiFiClient c = srv.available();
  if(!c) return;
  _client(c);
  c.stop();
}

void YoDlna::_client(WiFiClient& c){
  /*  заголовки  */
  char* head = hdrBuf; size_t hn = 0;
  uint32_t t0 = millis();
  bool done = false;
  while(millis() - t0 < 3000 && hn < HDR - 1){
    if(!c.connected() && !c.available()) break;
    int r = c.read();
    if(r < 0){ vTaskDelay(pdMS_TO_TICKS(2)); continue; }
    head[hn++] = (char)r;
    if(hn >= 4 && !memcmp(head + hn - 4, "\r\n\r\n", 4)){ done = true; break; }
  }
  head[hn] = 0;
  if(!done) return;

  char method[16] = {0}, path[160] = {0};
  sscanf(head, "%15s %159s", method, path);
  size_t clen = 0;
  const char* cl = strcasestr(head, "content-length:");
  if(cl) clen = (size_t)atoi(cl + 15);
  char action[64] = {0};
  const char* sa = strcasestr(head, "soapaction:");
  if(sa){
    const char* h = strchr(sa, '#');
    if(h){ size_t i = 0; h++; while(h[i] && h[i] != '"' && h[i] != '\r' && i < sizeof(action) - 1){ action[i] = h[i]; i++; } action[i] = 0; }
  }
  /*  ім'я того, хто керує — щоб показати на екрані  */
  const char* ua = strcasestr(head, "user-agent:");
  if(ua){
    ua += 11; while(*ua == ' ') ua++;
    size_t i = 0;
    while(ua[i] && ua[i] != '\r' && ua[i] != '\n' && ua[i] != '/' && i < sizeof(_device) - 1){ _device[i] = ua[i]; i++; }
    while(i && _device[i-1] == ' ') i--;
    _device[i] = 0;
  }

  char* body = bigBuf;
  body[0] = 0;
  if(clen){
    size_t got = 0, cap = BIG - 1;
    if(clen > cap) clen = cap;
    t0 = millis();
    while(got < clen && millis() - t0 < 5000){
      int r = c.read((uint8_t*)body + got, clen - got);
      if(r > 0){ got += r; t0 = millis(); }
      else vTaskDelay(pdMS_TO_TICKS(2));
    }
    body[got] = 0;
  }

  if(!strcmp(method, "SUBSCRIBE")){
    /*  Подій ми не шлемо (керівники й так перепитують стан), але без відповіді
        дехто не показує пристрій узагалі.  */
    char extra[128];
    snprintf(extra, sizeof(extra), "SID: uuid:%s\r\nTIMEOUT: Second-1800\r\n", _uuid);
    sendHead(c, 200, "text/plain", 0, extra);
    return;
  }
  if(!strcmp(method, "UNSUBSCRIBE")){ sendHead(c, 200, "text/plain", 0); return; }

  if(!strcmp(method, "GET") || !strcmp(method, "HEAD")){
    if(!strcmp(path, "/desc.xml")){
      char* b = outBuf;
      snprintf(b, OUT, DESC, friendlyOf(), prVersion(), _uuid, _uuid);
      sendBody(c, 200, "text/xml; charset=\"utf-8\"", b);
      return;
    }
    if(!strcmp(path, "/avt.xml")){ sendBody(c, 200, "text/xml; charset=\"utf-8\"", SCPD_AVT); return; }
    if(!strcmp(path, "/rc.xml")){  sendBody(c, 200, "text/xml; charset=\"utf-8\"", SCPD_RC);  return; }
    if(!strcmp(path, "/cm.xml")){  sendBody(c, 200, "text/xml; charset=\"utf-8\"", SCPD_CM);  return; }
    sendBody(c, 404, "text/plain", "немає");
    return;
  }

  if(!strcmp(method, "POST")){
    if(!strncmp(path, "/ctl/avt", 8)){ _avt(c, action, body); return; }
    if(!strncmp(path, "/ctl/rc", 7)){  _rc(c, action, body);  return; }
    if(!strncmp(path, "/ctl/cm", 7)){  _cm(c, action, body);  return; }
    sendBody(c, 404, "text/plain", "немає");
    return;
  }
  sendBody(c, 400, "text/plain", "не зрозуміло");
}

/*  ---------- дії ---------- */

void YoDlna::_play(){
  if(!_uri[0]) return;
  strlcpy(player.burl, _uri, sizeof(player.burl));
  strlcpy(player.burlTitle, _title[0] ? _title : "Бездротова колонка", sizeof(player.burlTitle));
  /*  Колонка після доріжки не вмикає станцію сама: далі керує той, хто надіслав.  */
  player.burlResumeRadio = 0;
  if(player.status() == PLAYING) player.sendCommand({PR_STOP, 0});
  _playing = true; _startMs = millis();
  player.sendCommand({PR_BURL, 0});
  Serial.printf("##DLNA#\tграю: %s (%s)\n", _title[0] ? _title : "без назви", _device[0] ? _device : "?");
}

void YoDlna::_avt(WiFiClient& c, const char* action, const char* body){
  if(!strcmp(action, "SetAVTransportURI")){
    char uri[512] = {0};
    if(tagValue(body, "CurrentURI", uri, sizeof(uri))){
      unescape(uri);
      strlcpy(_uri, uri, sizeof(_uri));
      _title[0] = 0;
      /*  Опис доріжки приходить екранованим; розбираємо його в буфері відповіді,
          щоб не тримати кілька кілобайтів на стеку.  */
      if(tagValue(body, "CurrentURIMetaData", outBuf, OUT)){
        unescape(outBuf);
        char t[96] = {0};
        if(tagValue(outBuf, "dc:title", t, sizeof(t)) || tagValue(outBuf, "title", t, sizeof(t))) strlcpy(_title, t, sizeof(_title));
      }
    }
    soapOk(c, "AVTransport", action, "");
    return;
  }
  if(!strcmp(action, "Play")){ _play(); soapOk(c, "AVTransport", action, ""); return; }
  if(!strcmp(action, "Stop") || !strcmp(action, "Pause")){
    if(player.status() == PLAYING) player.sendCommand({PR_STOP, 0});
    _playing = false;
    soapOk(c, "AVTransport", action, "");
    return;
  }
  if(!strcmp(action, "GetTransportInfo")){
    const char* st = (_playing && player.status() == PLAYING) ? "PLAYING" : (_playing ? "TRANSITIONING" : "STOPPED");
    char args[200];
    snprintf(args, sizeof(args), "<CurrentTransportState>%s</CurrentTransportState>"
             "<CurrentTransportStatus>OK</CurrentTransportStatus><CurrentSpeed>1</CurrentSpeed>", st);
    soapOk(c, "AVTransport", action, args);
    return;
  }
  if(!strcmp(action, "GetPositionInfo") || !strcmp(action, "GetMediaInfo")){
    char uri[560]; escape(_uri, uri, sizeof(uri));      /* невеликі — на стеку можна */
    char args[900];
    if(!strcmp(action, "GetPositionInfo"))
      snprintf(args, sizeof(args),
        "<Track>1</Track><TrackDuration>00:00:00</TrackDuration><TrackMetaData></TrackMetaData>"
        "<TrackURI>%s</TrackURI><RelTime>00:00:00</RelTime><AbsTime>00:00:00</AbsTime>"
        "<RelCount>2147483647</RelCount><AbsCount>2147483647</AbsCount>", uri);
    else
      snprintf(args, sizeof(args),
        "<NrTracks>1</NrTracks><MediaDuration>00:00:00</MediaDuration><CurrentURI>%s</CurrentURI>"
        "<CurrentURIMetaData></CurrentURIMetaData><NextURI></NextURI><NextURIMetaData></NextURIMetaData>"
        "<PlayMedium>NETWORK</PlayMedium><RecordMedium>NOT_IMPLEMENTED</RecordMedium>"
        "<WriteStatus>NOT_IMPLEMENTED</WriteStatus>", uri);
    soapOk(c, "AVTransport", action, args);
    return;
  }
  if(!strcmp(action, "GetDeviceCapabilities")){
    soapOk(c, "AVTransport", action, "<PlayMedia>NETWORK,HDD</PlayMedia><RecMedia>NOT_IMPLEMENTED</RecMedia>"
                                     "<RecQualityModes>NOT_IMPLEMENTED</RecQualityModes>");
    return;
  }
  if(!strcmp(action, "GetTransportSettings")){
    soapOk(c, "AVTransport", action, "<PlayMode>NORMAL</PlayMode><RecQualityMode>NOT_IMPLEMENTED</RecQualityMode>");
    return;
  }
  if(!strcmp(action, "SetNextAVTransportURI") || !strcmp(action, "SetPlayMode")){ soapOk(c, "AVTransport", action, ""); return; }
  if(!strcmp(action, "Seek")){ soapErr(c, 710, "Seek mode not supported"); return; }
  soapErr(c, 401, "Invalid Action");
}

void YoDlna::_rc(WiFiClient& c, const char* action, const char* body){
  if(!strcmp(action, "GetVolume")){
    char args[64]; snprintf(args, sizeof(args), "<CurrentVolume>%u</CurrentVolume>", (unsigned)volTo100());
    soapOk(c, "RenderingControl", action, args);
    return;
  }
  if(!strcmp(action, "SetVolume")){
    char v[16] = {0};
    if(tagValue(body, "DesiredVolume", v, sizeof(v))){
      player.setVol(volFrom100(atoi(v)));
      _mute = 0;
    }
    soapOk(c, "RenderingControl", action, "");
    return;
  }
  if(!strcmp(action, "GetMute")){
    char args[48]; snprintf(args, sizeof(args), "<CurrentMute>%u</CurrentMute>", (unsigned)(_mute ? 1 : 0));
    soapOk(c, "RenderingControl", action, args);
    return;
  }
  if(!strcmp(action, "SetMute")){
    char v[8] = {0};
    if(tagValue(body, "DesiredMute", v, sizeof(v))){
      bool m = (v[0] == '1' || v[0] == 't' || v[0] == 'T');
      if(m && !_mute){ _muteVol = config.store.volume; _mute = 1; player.setVol(0); }
      else if(!m && _mute){ _mute = 0; player.setVol(_muteVol ? _muteVol : 60); }
    }
    soapOk(c, "RenderingControl", action, "");
    return;
  }
  if(!strcmp(action, "ListPresets")){ soapOk(c, "RenderingControl", action, "<CurrentPresetNameList>FactoryDefaults</CurrentPresetNameList>"); return; }
  if(!strcmp(action, "SelectPreset")){ soapOk(c, "RenderingControl", action, ""); return; }
  soapErr(c, 401, "Invalid Action");
}

void YoDlna::_cm(WiFiClient& c, const char* action, const char* body){
  (void)body;
  if(!strcmp(action, "GetProtocolInfo")){
    char* b = (char*)malloc(strlen(SINK) + 48);
    if(!b){ soapErr(c, 501, "no memory"); return; }
    sprintf(b, "<Source></Source><Sink>%s</Sink>", SINK);
    soapOk(c, "ConnectionManager", action, b);
    free(b);
    return;
  }
  if(!strcmp(action, "GetCurrentConnectionIDs")){ soapOk(c, "ConnectionManager", action, "<ConnectionIDs>0</ConnectionIDs>"); return; }
  if(!strcmp(action, "GetCurrentConnectionInfo")){
    soapOk(c, "ConnectionManager", action,
      "<RcsID>0</RcsID><AVTransportID>0</AVTransportID><ProtocolInfo></ProtocolInfo>"
      "<PeerConnectionManager></PeerConnectionManager><PeerConnectionID>-1</PeerConnectionID>"
      "<Direction>Input</Direction><Status>OK</Status>");
    return;
  }
  soapErr(c, 401, "Invalid Action");
}
