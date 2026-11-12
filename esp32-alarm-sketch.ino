// =====================================================================
// ESP32 Clock / Calendar / Alarm
// =====================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <RTClib.h>
#include <time.h>

// ---------------- PIN MAP ----------------
static constexpr uint8_t PIN_SDA      = 21;
static constexpr uint8_t PIN_SCL      = 22;
static constexpr uint8_t PIN_ALARM    = 25;
static constexpr uint8_t PIN_BOOT_BTN = 0;

// ---------------- 64-BIT CONSTANTS ----------------
static constexpr int64_t  MS_PER_SEC     = 1000LL;
static constexpr int64_t  MS_PER_MIN     = 60LL  * MS_PER_SEC;
static constexpr int64_t  MS_PER_HOUR    = 60LL  * MS_PER_MIN;
static constexpr int64_t  MS_PER_DAY     = 24LL  * MS_PER_HOUR;
static constexpr uint32_t BOOT_HOLD_MS   = 5000UL;
static constexpr uint32_t ALARM_PULSE_MS = 60000UL;
static constexpr uint32_t NVS_MIN_INTERVAL_MS = 300000UL;
static constexpr uint32_t NVS_FORCE_MS   = 3600000UL;
static constexpr uint32_t BTN_DEBOUNCE_MS = 30UL;
static constexpr uint32_t AP_DEFER_MS    = 50UL;
static constexpr size_t   SETTIME_BODY_CAP = 512;
static constexpr size_t   TZ_JSON_BUF     = 224;
static constexpr int64_t  UNIX_MIN = 1577836800LL;
static constexpr int64_t  UNIX_MAX = 4133980800LL;

// NVS schema versions
static constexpr uint8_t  NVS_VER_ALARMS = 1;
static constexpr uint8_t  NVS_VER_APCFG  = 1;
static constexpr uint8_t  NVS_VER_RTCCFG = 1;

// ---------------- PROGMEM DEFAULTS ----------------
static const char DEF_AP_SSID[]  PROGMEM = "ESP32_Alarm_Clock";
static const char DEF_AP_PASS[]  PROGMEM = "12345678";
static const char DEF_TZ[]       PROGMEM = "UTC0";

// ---------------- GLOBALS ----------------
RTC_DS3231   rtc;
WebServer    server(80);
DNSServer    dns;
Preferences  prefs;
bool rtcOk  = false;
bool apMode = false;
struct AlarmSlot {
  uint8_t hour    = 0;
  uint8_t minute  = 0;
  uint8_t days    = 0;
  uint8_t enabled = 0;
  uint8_t _pad[4] = {0};
};
static constexpr uint8_t ALARM_COUNT = 8;
AlarmSlot alarms[ALARM_COUNT];
struct ApSettings {
  char    ssid[33];
  char    pass[65];
  uint8_t channel;
  uint8_t hidden;
  uint8_t maxConn;
  uint8_t _pad;
};
ApSettings apCfg;
struct RtcSettings {
  int8_t   agingOffset;
  uint8_t  hour24;
  uint8_t  showSecs;
  uint8_t  _pad[5];
  char     tz[32];
};
RtcSettings rtcCfg;
uint64_t bootBtnPressMs     = 0;
uint64_t bootBtnDebounceMs  = 0;
bool     bootBtnHeld        = false;
bool     bootBtnDebouncing  = false;
uint64_t alarmActiveUntilMs = 0;
uint8_t  firedMask          = 0;
int8_t   firedHour          = -1;
int8_t   firedMinute        = -1;
uint64_t lastNvsWriteMs     = 0;
bool     dirtyAlarms  = false;
bool     dirtyRtc     = false;
bool     dirtyAp      = false;
bool     dirtyAny     = false;
uint64_t dirtySinceMs = 0;
bool     pendingApRestart   = false;
uint64_t pendingApRestartMs = 0;

// =====================================================================
// PROGMEM HELPER
// =====================================================================
static void copyProgmem(char* dst, const char* srcProgmem, size_t dstLen) {
  size_t i = 0;
  for (; i + 1 < dstLen; ++i) {
    char c = (char)pgm_read_byte(srcProgmem + i);
    dst[i] = c;
    if (c == 0) return;
  }
  dst[dstLen - 1] = 0;
}

// =====================================================================
// TIMEZONE STRING VALIDATION
// =====================================================================
static bool tzIsValid(const char* s) {
  for (size_t i = 0; s[i]; ++i) {
    uint8_t c = (uint8_t)s[i];
    bool ok =
      (c >= 'A' && c <= 'Z') ||
      (c >= 'a' && c <= 'z') ||
      (c >= '0' && c <= '9') ||
      c == '_' || c == '+' || c == ':' || c == '/' ||
      c == '-' || c == ' ';
    if (!ok) return false;
  }
  return true;
}

// =====================================================================
// JSON STRING ESCAPER
// =====================================================================
static void jsonEscape(const char* src, char* dst, size_t dstLen) {
  if (dstLen == 0) return;
  size_t j = 0;
  for (size_t i = 0; src[i] && j + 7 < dstLen; ++i) {
    uint8_t c = (uint8_t)src[i];
    if (c == '"' || c == '\\') {
      dst[j++] = '\\'; dst[j++] = (char)c;
    } else if (c == '\n') {
      dst[j++] = '\\'; dst[j++] = 'n';
    } else if (c == '\r') {
      dst[j++] = '\\'; dst[j++] = 'r';
    } else if (c == '\t') {
      dst[j++] = '\\'; dst[j++] = 't';
    } else if (c < 0x20 || c > 0x7E) {
      if (c < 0x20) {
        const char* hex = "0123456789abcdef";
        dst[j++] = '\\'; dst[j++] = 'u'; dst[j++] = '0'; dst[j++] = '0';
        dst[j++] = hex[(c >> 4) & 0xF];
        dst[j++] = hex[c & 0xF];
      } else {
        dst[j++] = (char)c;
      }
    } else {
      dst[j++] = (char)c;
    }
  }
  dst[j] = 0;
}

// =====================================================================
// HTML ESCAPER
// =====================================================================
static void htmlEscape(const char* src, char* dst, size_t dstLen) {
  if (dstLen == 0) return;
  size_t j = 0;
  for (size_t i = 0; src[i] && j + 6 < dstLen; ++i) {
    char c = src[i];
    if (c == '&')       { memcpy(dst + j, "&amp;",  5); j += 5; }
    else if (c == '\'') { memcpy(dst + j, "&#39;",  5); j += 5; }
    else if (c == '"')  { memcpy(dst + j, "&quot;", 6); j += 6; }
    else if (c == '<')  { memcpy(dst + j, "&lt;",   4); j += 4; }
    else if (c == '>')  { memcpy(dst + j, "&gt;",   4); j += 4; }
    else dst[j++] = c;
  }
  dst[j] = 0;
}

// =====================================================================
// RTC HELPERS
// =====================================================================
static bool rtcRead(DateTime& out) {
  if (!rtcOk) return false;
  out = rtc.now();
  uint16_t y = out.year();
  uint8_t  mo = out.month();
  uint8_t  d  = out.day();
  uint8_t  h  = out.hour();
  uint8_t  mi = out.minute();
  uint8_t  s  = out.second();
  if (y < 2020 || y > 2100) return false;
  if (mo < 1 || mo > 12)    return false;
  if (d  < 1 || d  > 31)    return false;
  if (h  > 23)              return false;
  if (mi > 59)              return false;
  if (s  > 59)              return false;
  return true;
}

static bool rtcWriteUnix(int64_t unixTs) {
  if (!rtcOk) return false;
  if (unixTs < UNIX_MIN || unixTs > UNIX_MAX) return false;
  rtc.adjust(DateTime((uint32_t)unixTs));
  return true;
}

static bool ds3231SetAging(int8_t offset) {
  if (!rtcOk) return false;
  Wire.beginTransmission(0x68);
  Wire.write((uint8_t)0x10);
  Wire.write((uint8_t)offset);
  return Wire.endTransmission() == 0;
}

static bool ds3231GetAging(int8_t& out) {
  if (!rtcOk) return false;
  Wire.beginTransmission(0x68);
  Wire.write((uint8_t)0x10);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)0x68, (uint8_t)1) != 1) return false;
  out = (int8_t)Wire.read();
  return true;
}

// =====================================================================
// CONFIG LOAD
// =====================================================================
static void loadAlarms() {
  memset(alarms, 0, sizeof(alarms));
  prefs.begin("alarms", true);
  if (prefs.getUChar("ver", 0) == NVS_VER_ALARMS &&
      prefs.getBytesLength("slots") == sizeof(alarms)) {
    prefs.getBytes("slots", alarms, sizeof(alarms));
  }
  prefs.end();
}

static void loadApCfg() {
  memset(&apCfg, 0, sizeof(apCfg));
  prefs.begin("apcfg", true);
  if (prefs.getUChar("ver", 0) == NVS_VER_APCFG &&
      prefs.getBytesLength("cfg") == sizeof(apCfg)) {
    prefs.getBytes("cfg", &apCfg, sizeof(apCfg));
  }
  prefs.end();
  bool ssidOk = apCfg.ssid[0] != 0 && apCfg.ssid[32] == 0 &&
                strnlen(apCfg.ssid, sizeof(apCfg.ssid)) <= 32;
  bool passOk = strnlen(apCfg.pass, sizeof(apCfg.pass)) >= 8 &&
                strnlen(apCfg.pass, sizeof(apCfg.pass)) <= 63 &&
                apCfg.pass[64] == 0;
  if (!ssidOk) {
    memset(apCfg.ssid, 0, sizeof(apCfg.ssid));
    copyProgmem(apCfg.ssid, DEF_AP_SSID, sizeof(apCfg.ssid));
  }
  if (!passOk) {
    memset(apCfg.pass, 0, sizeof(apCfg.pass));
    copyProgmem(apCfg.pass, DEF_AP_PASS, sizeof(apCfg.pass));
  }
  if (apCfg.channel < 1 || apCfg.channel > 13) apCfg.channel = 6;
  if (apCfg.maxConn < 1 || apCfg.maxConn > 8) apCfg.maxConn = 4;
  apCfg.hidden = apCfg.hidden ? 1 : 0;
}

static void loadRtcCfg() {
  memset(&rtcCfg, 0, sizeof(rtcCfg));
  prefs.begin("rtccfg", true);
  if (prefs.getUChar("ver", 0) == NVS_VER_RTCCFG &&
      prefs.getBytesLength("cfg") == sizeof(rtcCfg)) {
    prefs.getBytes("cfg", &rtcCfg, sizeof(rtcCfg));
  }
  prefs.end();
  if (rtcCfg.hour24 > 1) rtcCfg.hour24 = 1;
  if (rtcCfg.showSecs > 1) rtcCfg.showSecs = 1;
  if (rtcCfg.agingOffset < -128 || rtcCfg.agingOffset > 127) rtcCfg.agingOffset = 0;
  if (rtcCfg.tz[31] != 0 || rtcCfg.tz[0] == 0 || !tzIsValid(rtcCfg.tz)) {
    memset(rtcCfg.tz, 0, sizeof(rtcCfg.tz));
    copyProgmem(rtcCfg.tz, DEF_TZ, sizeof(rtcCfg.tz));
  }
  if (rtcOk) ds3231SetAging(rtcCfg.agingOffset);
}

// =====================================================================
// DEFERRED NVS FLUSH
// =====================================================================
static void markDirty(bool& flag, uint64_t nowMs) {
  if (!dirtyAny) {
    dirtyAny = true;
    dirtySinceMs = nowMs;
  } else if ((int64_t)(nowMs - dirtySinceMs) < 0) {
    dirtySinceMs = nowMs;
  }
  flag = true;
}

static void serviceNvsFlush(uint64_t nowMs) {
  if (!dirtyAlarms && !dirtyRtc && !dirtyAp) {
    dirtyAny = false;
    return;
  }
  bool intervalOk = (nowMs - lastNvsWriteMs >= NVS_MIN_INTERVAL_MS) &&
                    (nowMs - dirtySinceMs  >= NVS_MIN_INTERVAL_MS);
  bool forced     = (nowMs - dirtySinceMs  >= NVS_FORCE_MS);
  if (!intervalOk && !forced) return;
  if (dirtyAlarms) {
    prefs.begin("alarms", false);
    prefs.putUChar("ver", NVS_VER_ALARMS);
    prefs.putBytes("slots", alarms, sizeof(alarms));
    prefs.end();
    dirtyAlarms = false;
    lastNvsWriteMs = nowMs;
  }
  if (dirtyRtc) {
    prefs.begin("rtccfg", false);
    prefs.putUChar("ver", NVS_VER_RTCCFG);
    prefs.putBytes("cfg", &rtcCfg, sizeof(rtcCfg));
    prefs.end();
    dirtyRtc = false;
    lastNvsWriteMs = nowMs;
  }
  if (dirtyAp) {
    prefs.begin("apcfg", false);
    prefs.putUChar("ver", NVS_VER_APCFG);
    prefs.putBytes("cfg", &apCfg, sizeof(apCfg));
    prefs.end();
    dirtyAp = false;
    lastNvsWriteMs = nowMs;
  }
  if (!dirtyAlarms && !dirtyRtc && !dirtyAp) dirtyAny = false;
}

// =====================================================================
// AP START
// =====================================================================
static void startAp() {
  char ssid[33]; char pass[65];
  memcpy(ssid, apCfg.ssid, sizeof(ssid)); ssid[32] = 0;
  memcpy(pass, apCfg.pass, sizeof(pass)); pass[64] = 0;
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1),
                    IPAddress(255,255,255,0));
  bool ok = WiFi.softAP(ssid, pass, apCfg.channel,
                        apCfg.hidden ? true : false, apCfg.maxConn);
  if (!ok) {
    char dSsid[33]; char dPass[65];
    copyProgmem(dSsid, DEF_AP_SSID, sizeof(dSsid));
    copyProgmem(dPass, DEF_AP_PASS, sizeof(dPass));
    WiFi.softAP(dSsid, dPass, 6, false, 4);
  }
  dns.start(53, "*", WiFi.softAPIP());
  apMode = true;
}

// =====================================================================
// ALARM ENGINE
// =====================================================================
static void checkAlarms(uint64_t nowMs) {
  if (alarmActiveUntilMs && nowMs >= alarmActiveUntilMs) {
    digitalWrite(PIN_ALARM, LOW);
    alarmActiveUntilMs = 0;
  }
  DateTime dt;
  if (!rtcRead(dt)) return;
  uint8_t hour   = dt.hour();
  uint8_t minute = dt.minute();
  uint8_t dow    = dt.dayOfTheWeek();
  if (hour != (uint8_t)firedHour || minute != (uint8_t)firedMinute) {
    firedHour   = (int8_t)hour;
    firedMinute = (int8_t)minute;
    firedMask   = 0;
  }
  for (uint8_t i = 0; i < ALARM_COUNT; ++i) {
    if (firedMask & (1u << i)) continue;
    const AlarmSlot& a = alarms[i];
    if (!a.enabled) continue;
    if (a.hour != hour || a.minute != minute) continue;
    if (!((a.days >> dow) & 1u)) continue;
    digitalWrite(PIN_ALARM, HIGH);
    uint64_t newEnd = nowMs + ALARM_PULSE_MS;
    if (newEnd > alarmActiveUntilMs) alarmActiveUntilMs = newEnd;
    firedMask |= (1u << i);
    break;
  }
}

// =====================================================================
// BOOT BUTTON
// =====================================================================
static void serviceBootButton(uint64_t nowMs) {
  bool pressed = (digitalRead(PIN_BOOT_BTN) == LOW);
  if (pressed) {
    if (!bootBtnDebouncing) {
      bootBtnDebouncing = true;
      bootBtnDebounceMs = nowMs;
    } else if (!bootBtnHeld && (nowMs - bootBtnDebounceMs >= BTN_DEBOUNCE_MS)) {
      bootBtnHeld = true;
      bootBtnPressMs = nowMs;
    } else if (bootBtnHeld && (nowMs - bootBtnPressMs >= BOOT_HOLD_MS)) {
      prefs.begin("alarms", false); prefs.clear(); prefs.end();
      prefs.begin("apcfg",  false); prefs.clear(); prefs.end();
      prefs.begin("rtccfg", false); prefs.clear(); prefs.end();
      ESP.restart();
    }
  } else {
    bootBtnDebouncing = false;
    bootBtnHeld = false;
  }
}

// =====================================================================
// WEB UI SHARED
// =====================================================================
static const char PROGMEM HTML_HEAD[] =
"<!doctype html><html lang='en'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1,viewport-fit=cover'>"
"<meta name='color-scheme' content='dark light'>"
"<title>ESP32 Clock</title><style>"
"*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}"
"html,body{margin:0;padding:0}"
"body{font-family:system-ui,-apple-system,'Segoe UI',Roboto,Arial,sans-serif;"
"background:#111;color:#eee;min-height:100vh;overflow-x:hidden}"
"header{background:#222;padding:8px 12px;display:flex;gap:6px;"
"align-items:center;flex-wrap:wrap;position:sticky;top:0;z-index:10;"
"padding-top:calc(8px + env(safe-area-inset-top))}"
"header a{color:#8cf;text-decoration:none;font-weight:600;padding:10px 14px;"
"border-radius:8px;min-height:44px;display:inline-flex;align-items:center;"
"font-size:15px}"
"header a:hover,header a:focus,header a:active{background:#333}"
"header a.active{background:#2a6;color:#fff}"
"main{max-width:920px;margin:0 auto;padding:16px;"
"padding-bottom:calc(80px + env(safe-area-inset-bottom))}"
".card{background:#1c1c1c;border:1px solid #333;border-radius:12px;"
"padding:16px;margin:12px 0}"
".clock{font-size:clamp(36px,12vw,80px);font-weight:700;letter-spacing:2px;"
"text-align:center;font-variant-numeric:tabular-nums;line-height:1.05;"
"font-feature-settings:'tnum' 1}"
".date{text-align:center;font-size:clamp(16px,4.5vw,24px);color:#9cf;margin-top:6px}"
".tz{text-align:center;font-size:13px;color:#888;margin-top:4px}"
"label{display:block;margin:10px 0 4px;font-size:14px;color:#bbb}"
"input,select{width:100%;padding:12px;font-size:16px;border-radius:8px;"
"border:1px solid #444;background:#0d0d0d;color:#eee;font-family:inherit;"
"min-height:44px;-webkit-appearance:none;appearance:none}"
"input:focus,select:focus{outline:2px solid #8cf;outline-offset:0}"
"button{padding:14px 18px;font-size:16px;border-radius:10px;border:0;"
"background:#2a6;color:#fff;font-weight:700;min-height:48px;width:100%;"
"margin-top:12px;cursor:pointer;font-family:inherit;touch-action:manipulation}"
"button:active{transform:scale(0.98)}"
"button.secondary{background:#2a4f8a}"
"button.danger{background:#a33}"
".row{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px}"
".slot{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;"
"align-items:end;padding:10px;border:1px solid #333;border-radius:8px;margin:8px 0}"
".slot h4{margin:0;grid-column:1/-1;color:#8cf;font-size:14px}"
".days{display:flex;gap:4px;flex-wrap:wrap;grid-column:1/-1}"
".days label{display:inline-flex;align-items:center;gap:4px;font-size:13px;"
"margin:0;padding:6px 8px;border:1px solid #444;border-radius:6px;"
"cursor:pointer;min-height:36px}"
".days input{width:16px;height:16px;padding:0;min-height:0;margin:0}"
".days input:checked+span{color:#8f8;font-weight:700}"
".status{display:flex;gap:12px;flex-wrap:wrap;font-size:14px;color:#aaa}"
".status b{color:#eee}"
".toast{position:fixed;left:50%;"
"bottom:calc(20px + env(safe-area-inset-bottom));transform:translateX(-50%);"
"background:#2a6;color:#fff;padding:12px 18px;border-radius:10px;"
"font-size:15px;opacity:0;transition:opacity .25s;pointer-events:none;"
"z-index:100;max-width:90vw;text-align:center}"
".toast.show{opacity:1}"
"@media(max-width:480px){.slot{grid-template-columns:1fr 1fr}}"
"</style></head><body>"
"<header>"
"<a href='/' id='nav-home'>Dashboard</a>"
"<a href='/alarms' id='nav-alarms'>Alarms</a>"
"<a href='/rtc' id='nav-rtc'>RTC</a>"
"<a href='/wifi' id='nav-wifi'>WiFi AP</a>"
"</header><main>";

static const char PROGMEM HTML_TAIL[] =
"<div class='toast' id='toast'></div>"
"<script>"
"function toast(msg){var t=document.getElementById('toast');"
"if(!t)return;t.textContent=msg;t.classList.add('show');"
"clearTimeout(t._timer);t._timer=setTimeout(function(){"
"t.classList.remove('show')},2500)}"
"(function(){var p=location.pathname;"
"if(p.length>1&&p.charAt(p.length-1)==='/'){p=p.substring(0,p.length-1)}"
"var m={'/':'nav-home','/alarms':'nav-alarms','/rtc':'nav-rtc','/wifi':'nav-wifi'};"
"var k=m[p];if(k){var el=document.getElementById(k);if(el)el.classList.add('active')}"
"})();"
"</script></main></body></html>";

// =====================================================================
// DASHBOARD
// =====================================================================
static void handleRoot() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  server.sendContent_P(HTML_HEAD);
  DateTime dt;
  bool ok = rtcRead(dt);
  char buf[600];

  server.sendContent_P(
    "<div class='card'>"
    "<div class='clock' id='clk'>--:--:--</div>"
    "<div class='date' id='dt'>loading</div>"
    "<div class='tz' id='tzl'></div>"
    "</div>");

  server.sendContent_P("<div class='card'><div class='status'>");
  server.sendContent_P(ok ? "<span>RTC: <b>OK</b></span>"
                          : "<span>RTC: <b>NOT FOUND</b></span>");
  if (ok) {
    snprintf(buf, sizeof(buf),
      "<span>Time: <b>%02u:%02u:%02u</b></span>"
      "<span>Date: <b>%02u/%02u/%04u</b></span>",
      (unsigned)dt.hour(), (unsigned)dt.minute(), (unsigned)dt.second(),
      (unsigned)dt.month(), (unsigned)dt.day(), (unsigned)dt.year());
    server.sendContent(buf);
  }
  char rootSsidEsc[200];
  htmlEscape(WiFi.softAPSSID().c_str(), rootSsidEsc, sizeof(rootSsidEsc));
  snprintf(buf, sizeof(buf),
    "<span>AP: <b>%s</b></span><span>IP: <b>%s</b></span>"
    "<span>Clients: <b>%u</b></span></div></div>",
    rootSsidEsc,
    WiFi.softAPIP().toString().c_str(),
    (unsigned)WiFi.softAPgetStationNum());
  server.sendContent(buf);

  server.sendContent_P(
    "<div class='card'>"
    "<h3 style='margin:0 0 6px'>Sync Clock from this Device</h3>"
    "<p style='color:#aaa;font-size:14px;margin:0 0 6px'>"
    "Sets the DS3231 to this browser's current time and date.</p>"
    "<button type='button' id='syncBtn' class='secondary'>"
    "Sync DS3231 to Browser Time</button></div>");

  server.sendContent_P(
"<script>"
"function pad(n){return n<10?'0'+n:''+n}"
"function tick(){"
"fetch('/api/time',{cache:'no-store'})"
".then(function(r){return r.json()})"
".then(function(j){"
"if(j.ok){"
"var h=j.h;var ampm='';"
"if(!j.h24){ampm=(h<12?' AM':' PM');var h12=h%12;if(h12===0)h12=12;"
"document.getElementById('clk').textContent="
"pad(h12)+':'+pad(j.m)+(j.ss?':'+pad(j.s):'')+ampm;"
"}else{document.getElementById('clk').textContent="
"pad(h)+':'+pad(j.m)+(j.ss?':'+pad(j.s):'')}"
"var dn=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];"
"document.getElementById('dt').textContent="
"dn[j.dw]+' '+pad(j.mo)+'/'+pad(j.d)+'/'+j.y;"
"document.getElementById('tzl').textContent="
"j.tz?('TZ: '+j.tz):'';"
"}"
"}).catch(function(){}).then(function(){setTimeout(tick,1000)})"
"}"
"tick();"
"document.getElementById('syncBtn').addEventListener('click',function(){"
"var b=this;b.disabled=true;b.textContent='Syncing...';"
"var now=new Date();"
"var payload={epoch:Math.floor(now.getTime()/1000)};"
"fetch('/api/set-time',{method:'POST',"
"headers:{'Content-Type':'text/plain'},"
"body:JSON.stringify(payload)})"
".then(function(r){return r.json()})"
".then(function(j){"
"if(j.ok){toast('Clock synced to browser time')}"
"else{toast('Sync failed: '+(j.err||'unknown'))}"
"}).catch(function(){toast('Sync failed')})"
".then(function(){b.disabled=false;"
"b.textContent='Sync DS3231 to Browser Time'})"
"});"
"</script>");

  server.sendContent_P(HTML_TAIL);
}

// =====================================================================
// API — time read
// =====================================================================
static void handleApiTime() {
  DateTime dt;
  bool ok = rtcRead(dt);
  char esc[TZ_JSON_BUF];
  jsonEscape(rtcCfg.tz, esc, sizeof(esc));
  char out[384];
  if (!ok) {
    snprintf(out, sizeof(out), "{\"ok\":false}");
  } else {
    snprintf(out, sizeof(out),
      "{\"ok\":true,\"h\":%u,\"m\":%u,\"s\":%u,\"mo\":%u,\"d\":%u,\"y\":%u,"
      "\"dw\":%u,\"h24\":%u,\"ss\":%u,\"tz\":\"%s\"}",
      (unsigned)dt.hour(), (unsigned)dt.minute(), (unsigned)dt.second(),
      (unsigned)dt.month(), (unsigned)dt.day(), (unsigned)dt.year(),
      (unsigned)dt.dayOfTheWeek(),
      (unsigned)(rtcCfg.hour24 ? 1 : 0),
      (unsigned)(rtcCfg.showSecs ? 1 : 0),
      esc);
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

// =====================================================================
// API — browser time sync
// =====================================================================
static void handleApiSetTime() {
  if (!rtcOk) {
    server.send(200, "application/json", "{\"ok\":false,\"err\":\"RTC not found\"}");
    return;
  }

  String body = server.arg("plain");
  if (body.length() == 0 || body.length() > SETTIME_BODY_CAP) {
    server.send(200, "application/json", "{\"ok\":false,\"err\":\"empty or too large\"}");
    return;
  }

  int idx = body.indexOf("\"epoch\"");
  if (idx < 0) {
    server.send(200, "application/json", "{\"ok\":false,\"err\":\"missing epoch\"}");
    return;
  }
  int cursor = idx + 7;
  while (cursor < (int)body.length() &&
         (body[cursor] == ' ' || body[cursor] == '\t' || body[cursor] == '\n' ||
          body[cursor] == '\r')) cursor++;
  if (cursor >= (int)body.length() || body[cursor] != ':') {
    server.send(200, "application/json", "{\"ok\":false,\"err\":\"bad format\"}");
    return;
  }
  int start = cursor + 1;
  while (start < (int)body.length() &&
         (body[start] == ' ' || body[start] == '\t')) start++;
  bool negative = false;
  if (start < (int)body.length() && body[start] == '-') {
    negative = true;
    start++;
  }
  int end = start;
  while (end < (int)body.length() &&
         body[end] >= '0' && body[end] <= '9') end++;
  if (end == start) {
    server.send(200, "application/json", "{\"ok\":false,\"err\":\"no digits\"}");
    return;
  }
  int64_t epoch = (int64_t)atoll(body.substring(start, end).c_str());
  if (negative) epoch = -epoch;

  if (epoch < UNIX_MIN || epoch > UNIX_MAX) {
    server.send(200, "application/json", "{\"ok\":false,\"err\":\"epoch out of range\"}");
    return;
  }

  if (!rtcWriteUnix(epoch)) {
    server.send(200, "application/json", "{\"ok\":false,\"err\":\"write failed\"}");
    return;
  }
  DateTime v;
  if (!rtcRead(v)) {
    server.send(200, "application/json", "{\"ok\":false,\"err\":\"read failed\"}");
    return;
  }
  int64_t got = (int64_t)v.unixtime();
  if (got != epoch && got != epoch + 1 && got != epoch - 1) {
    server.send(200, "application/json", "{\"ok\":false,\"err\":\"verify mismatch\"}");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", "{\"ok\":true}");
}

// =====================================================================
// ALARMS PAGE
// =====================================================================
static const char DAY_NAMES[7][4] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static void handleAlarmsPage() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  server.sendContent_P(HTML_HEAD);
  server.sendContent_P("<div class='card'><h3 style='margin-top:0'>Alarm Schedules</h3>"
                       "<form method='POST' action='/alarms'>");
  char buf[700];
  for (uint8_t i = 0; i < ALARM_COUNT; ++i) {
    AlarmSlot& a = alarms[i];
    server.sendContent_P("<div class='slot'>");
    snprintf(buf, sizeof(buf), "<h4>Alarm %u</h4>", (unsigned)(i + 1));
    server.sendContent(buf);
    snprintf(buf, sizeof(buf),
      "<div><label>Time</label>"
      "<input name='t%u' type='time' value='%02u:%02u' required></div>",
      (unsigned)i, (unsigned)a.hour, (unsigned)a.minute);
    server.sendContent(buf);
    snprintf(buf, sizeof(buf),
      "<div><label>Enabled</label>"
      "<select name='e%u'><option value='0'%s>Off</option>"
      "<option value='1'%s>On</option></select></div>",
      (unsigned)i, a.enabled ? "" : " selected",
      a.enabled ? " selected" : "");
    server.sendContent(buf);
    server.sendContent_P("<div class='days'>");
    for (uint8_t d = 0; d < 7; ++d) {
      snprintf(buf, sizeof(buf),
        "<label><input type='checkbox' name='d%u_%u' value='1'%s>"
        "<span>%s</span></label>",
        (unsigned)i, (unsigned)d,
        ((a.days >> d) & 1u) ? " checked" : "",
        DAY_NAMES[d]);
      server.sendContent(buf);
    }
    server.sendContent_P("</div></div>");
  }
  server.sendContent_P("<button type='submit'>Save Alarms</button></form></div>");
  server.sendContent_P(HTML_TAIL);
}

static void handleAlarmsSave() {
  for (uint8_t i = 0; i < ALARM_COUNT; ++i) {
    char key[8];
    snprintf(key, sizeof(key), "t%u", (unsigned)i);
    String t = server.arg(key);
    int sep = t.indexOf(':');
    if (sep >= 1) {
      int hh = t.substring(0, sep).toInt();
      int mm = t.substring(sep + 1).toInt();
      if (hh >= 0 && hh < 24) alarms[i].hour   = (uint8_t)hh;
      if (mm >= 0 && mm < 60) alarms[i].minute = (uint8_t)mm;
    }
    snprintf(key, sizeof(key), "e%u", (unsigned)i);
    alarms[i].enabled = (server.arg(key) == "1") ? 1 : 0;
    uint8_t mask = 0;
    for (uint8_t d = 0; d < 7; ++d) {
      snprintf(key, sizeof(key), "d%u_%u", (unsigned)i, (unsigned)d);
      if (server.hasArg(key) && server.arg(key) == "1") mask |= (1u << d);
    }
    alarms[i].days = mask;
  }
  markDirty(dirtyAlarms, (uint64_t)millis());
  server.sendHeader("Location", "/alarms");
  server.send(303, "text/plain", "");
}

// =====================================================================
// RTC SETTINGS PAGE
// =====================================================================
static void handleRtcPage() {
  DateTime dt;
  bool ok = rtcRead(dt);
  int8_t aging = rtcCfg.agingOffset;
  int8_t liveAging = 0;
  bool liveAgingOk = ds3231GetAging(liveAging);
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  server.sendContent_P(HTML_HEAD);

  char buf[900];
  snprintf(buf, sizeof(buf),
    "<div class='card'><h3 style='margin-top:0'>DS3231 Settings</h3>"
    "<div class='status'>"
    "<span>Status: <b>%s</b></span>"
    "<span>Now: <b>%02u:%02u:%02u %02u/%02u/%04u</b></span>"
    "<span>Aging (chip): <b>%d</b></span>"
    "</div></div>",
    ok ? "OK" : "NOT FOUND",
    (unsigned)dt.hour(), (unsigned)dt.minute(), (unsigned)dt.second(),
    (unsigned)dt.month(), (unsigned)dt.day(), (unsigned)dt.year(),
    liveAgingOk ? (int)liveAging : 0);
  server.sendContent(buf);

  server.sendContent_P(
    "<div class='card'><h3 style='margin-top:0'>Sync from Browser</h3>"
    "<p style='color:#aaa;font-size:14px;margin:0'>"
    "Set DS3231 time and date to this browser's clock.</p>"
    "<button type='button' id='syncRtc' class='secondary'>Sync Now</button></div>");

  snprintf(buf, sizeof(buf),
    "<div class='card'><h3 style='margin-top:0'>Custom RTC Settings</h3>"
    "<form method='POST' action='/rtc'>"
    "<label for='aging'>Aging offset (-128 to 127; higher = slower)</label>"
    "<input id='aging' name='aging' type='number' min='-128' max='127' "
    "required value='%d'>"
    "<label for='h24'>Clock format</label>"
    "<select id='h24' name='h24'>"
    "<option value='1'%s>24-hour</option>"
    "<option value='0'%s>12-hour (AM/PM)</option></select>"
    "<label for='ss'>Show seconds</label>"
    "<select id='ss' name='ss'>"
    "<option value='1'%s>Yes</option>"
    "<option value='0'%s>No</option></select>"
    "<label for='tz'>Timezone label (display only)</label>"
    "<input id='tz' name='tz' maxlength='31' pattern='[A-Za-z0-9_+:\\-/ ]*' "
    "value='%s'>"
    "<button type='submit'>Save RTC Settings</button></form></div>",
    (int)aging,
    rtcCfg.hour24 ? " selected" : "",
    rtcCfg.hour24 ? "" : " selected",
    rtcCfg.showSecs ? " selected" : "",
    rtcCfg.showSecs ? "" : " selected",
    rtcCfg.tz);
  server.sendContent(buf);

  server.sendContent_P(
"<script>"
"document.getElementById('syncRtc').addEventListener('click',function(){"
"var b=this;b.disabled=true;b.textContent='Syncing...';"
"var now=new Date();"
"fetch('/api/set-time',{method:'POST',"
"headers:{'Content-Type':'text/plain'},"
"body:JSON.stringify({epoch:Math.floor(now.getTime()/1000)})})"
".then(function(r){return r.json()})"
".then(function(j){"
"if(j.ok){toast('DS3231 synced');setTimeout(function(){location.reload()},800)}"
"else{toast('Failed: '+(j.err||'unknown'))}"
"}).catch(function(){toast('Failed')})"
".then(function(){b.disabled=false;b.textContent='Sync Now'})"
"});"
"</script>");

  server.sendContent_P(HTML_TAIL);
}

static void handleRtcSave() {
  int aging = server.arg("aging").toInt();
  int h24   = server.arg("h24").toInt();
  int ss    = server.arg("ss").toInt();
  String tz = server.arg("tz");
  if (aging < -128 || aging > 127) {
    server.send(400, "text/plain", "Aging out of range");
    return;
  }
  if (tz.length() > 31) tz = tz.substring(0, 31);
  if (!tzIsValid(tz.c_str())) {
    server.send(400, "text/plain", "Timezone has invalid characters");
    return;
  }
  rtcCfg.agingOffset = (int8_t)aging;
  rtcCfg.hour24      = (h24 == 1) ? 1 : 0;
  rtcCfg.showSecs    = (ss  == 1) ? 1 : 0;
  memset(rtcCfg.tz, 0, sizeof(rtcCfg.tz));
  memcpy(rtcCfg.tz, tz.c_str(), tz.length());
  ds3231SetAging(rtcCfg.agingOffset);
  markDirty(dirtyRtc, (uint64_t)millis());
  server.sendHeader("Location", "/rtc");
  server.send(303, "text/plain", "");
}

// =====================================================================
// WIFI AP PAGE
// =====================================================================
static void handleWifiPage() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  server.sendContent_P(HTML_HEAD);

  char buf[1200];
  char wifiSsidEsc[200];
  char wifiPassEsc[400];
  char wifiLiveSsidEsc[200];
  htmlEscape(apCfg.ssid, wifiSsidEsc, sizeof(wifiSsidEsc));
  htmlEscape(apCfg.pass, wifiPassEsc, sizeof(wifiPassEsc));
  htmlEscape(WiFi.softAPSSID().c_str(), wifiLiveSsidEsc, sizeof(wifiLiveSsidEsc));

  snprintf(buf, sizeof(buf),
    "<div class='card'><h3 style='margin-top:0'>WiFi AP Settings</h3>"
    "<div class='status'>"
    "<span>IP: <b>%s</b></span>"
    "<span>Clients: <b>%u</b></span>"
    "<span>SSID: <b>%s</b></span>"
    "</div></div>",
    WiFi.softAPIP().toString().c_str(),
    (unsigned)WiFi.softAPgetStationNum(),
    wifiLiveSsidEsc);
  server.sendContent(buf);

  snprintf(buf, sizeof(buf),
    "<div class='card'><h3 style='margin-top:0'>Configure AP</h3>"
    "<form method='POST' action='/wifi' autocomplete='off'>"
    "<label for='ssid'>SSID (1-32 chars)</label>"
    "<input id='ssid' name='ssid' minlength='1' maxlength='32' required "
    "autocomplete='off' autocapitalize='off' spellcheck='false' value='%s'>"
    "<label for='pass'>Password (8-63 chars)</label>"
    "<input id='pass' name='pass' type='password' minlength='8' maxlength='63' "
    "required autocomplete='new-password' value='%s'>"
    "<div class='row'>"
    "<div><label for='ch'>Channel (1-13)</label>"
    "<input id='ch' name='ch' type='number' min='1' max='13' required "
    "value='%u'></div>"
    "<div><label for='mc'>Max clients (1-8)</label>"
    "<input id='mc' name='mc' type='number' min='1' max='8' required "
    "value='%u'></div>"
    "</div>"
    "<label for='hid'>Hidden SSID</label>"
    "<select id='hid' name='hid'>"
    "<option value='0'%s>No</option><option value='1'%s>Yes</option></select>"
    "<button type='submit' class='danger'>Save &amp; Restart AP</button>"
    "</form></div>",
    wifiSsidEsc, wifiPassEsc, (unsigned)apCfg.channel,
    (unsigned)apCfg.maxConn,
    apCfg.hidden ? "" : " selected",
    apCfg.hidden ? " selected" : "");
  server.sendContent(buf);
  server.sendContent_P(HTML_TAIL);
}

static void handleWifiSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  int ch  = server.arg("ch").toInt();
  int mc  = server.arg("mc").toInt();
  int hid = server.arg("hid").toInt();
  if (ssid.length() < 1 || ssid.length() > 32 ||
      pass.length() < 8 || pass.length() > 63 ||
      ch < 1 || ch > 13 || mc < 1 || mc > 8) {
    server.send(400, "text/plain", "Invalid AP settings");
    return;
  }
  memset(apCfg.ssid, 0, sizeof(apCfg.ssid));
  memset(apCfg.pass, 0, sizeof(apCfg.pass));
  memcpy(apCfg.ssid, ssid.c_str(), ssid.length());
  memcpy(apCfg.pass, pass.c_str(), pass.length());
  apCfg.channel = (uint8_t)ch;
  apCfg.maxConn = (uint8_t)mc;
  apCfg.hidden  = (uint8_t)(hid ? 1 : 0);
  prefs.begin("apcfg", false);
  prefs.putUChar("ver", NVS_VER_APCFG);
  prefs.putBytes("cfg", &apCfg, sizeof(apCfg));
  prefs.end();
  server.sendHeader("Location", "/wifi");
  server.send(303, "text/plain", "");
  pendingApRestart = true;
  pendingApRestartMs = (uint64_t)millis() + AP_DEFER_MS;
}

// =====================================================================
// CAPTIVE PORTAL
// =====================================================================
static void handleCaptiveProbe() {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

static void handleNotFound() {
  if (apMode) { handleCaptiveProbe(); return; }
  server.send(404, "text/plain", "Not found");
}

// =====================================================================
// SETUP
// =====================================================================
void setup() {
  pinMode(PIN_ALARM, OUTPUT);
  digitalWrite(PIN_ALARM, LOW);
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);
  rtcOk = rtc.begin(&Wire);
  if (rtcOk && rtc.lostPower()) {
    rtc.adjust(DateTime((uint32_t)UNIX_MIN));
  }
  loadAlarms();
  loadApCfg();
  loadRtcCfg();
  startAp();
  server.on("/",          HTTP_GET,  handleRoot);
  server.on("/api/time",  HTTP_GET,  handleApiTime);
  server.on("/api/set-time", HTTP_POST, handleApiSetTime);
  server.on("/alarms",    HTTP_GET,  handleAlarmsPage);
  server.on("/alarms",    HTTP_POST, handleAlarmsSave);
  server.on("/rtc",       HTTP_GET,  handleRtcPage);
  server.on("/rtc",       HTTP_POST, handleRtcSave);
  server.on("/wifi",      HTTP_GET,  handleWifiPage);
  server.on("/wifi",      HTTP_POST, handleWifiSave);
  server.on("/hotspot-detect.html",       HTTP_GET, handleCaptiveProbe);
  server.on("/library/test/success.html", HTTP_GET, handleCaptiveProbe);
  server.on("/generate_204",              HTTP_GET, handleCaptiveProbe);
  server.on("/gen_204",                   HTTP_GET, handleCaptiveProbe);
  server.on("/connecttest.txt",           HTTP_GET, handleCaptiveProbe);
  server.on("/ncsi.txt",                  HTTP_GET, handleCaptiveProbe);
  server.on("/redirect",                  HTTP_GET, handleCaptiveProbe);
  server.onNotFound(handleNotFound);
  server.begin();
  lastNvsWriteMs = (uint64_t)millis();
}

// =====================================================================
// LOOP
// =====================================================================
void loop() {
  const uint64_t nowMs = (uint64_t)millis();
  dns.processNextRequest();
  server.handleClient();
  serviceBootButton(nowMs);
  checkAlarms(nowMs);
  serviceNvsFlush(nowMs);
  if (pendingApRestart && nowMs >= pendingApRestartMs) {
    pendingApRestart = false;
    WiFi.softAPdisconnect(true);
    startAp();
  }
  delay(1);
}

