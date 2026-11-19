
#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <RTClib.h>
#include <ESPAsyncWebServer.h>
#include <esp_system.h>
#include <sys/time.h>
#include <time.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

static constexpr uint8_t I2C_SDA_PIN = 21;
static constexpr uint8_t I2C_SCL_PIN = 22;
static constexpr uint8_t ALARM_OUTPUT_PIN = 25;
static constexpr uint8_t FACTORY_RESET_PIN = 0;
static constexpr uint8_t MAX_ALARMS = 12;
static constexpr uint8_t MAX_EVENTS = 24;
static constexpr int64_t MIN_VALID_EPOCH = 1704067200LL;
static constexpr int64_t MAX_VALID_EPOCH = 4102444799LL;
static constexpr uint32_t CONFIG_MAGIC = 0x41434C4BUL;
static constexpr uint16_t CONFIG_VERSION = 2;
static constexpr uint32_t CONFIG_DEBOUNCE_MS = 5000UL;
static constexpr uint32_t CONFIG_MIN_WRITE_MS = 60000UL;
static constexpr uint32_t RTC_PROBE_INTERVAL_MS = 60000UL;
static constexpr uint32_t RTC_REFRESH_INTERVAL_SECONDS = 21600UL;

struct __attribute__((packed)) AlarmRecord {
  uint64_t id;
  char title[33];
  uint8_t enabled;
  uint8_t hour;
  uint8_t minute;
  uint8_t weekdays;
  uint32_t onceDate;
};

struct __attribute__((packed)) EventRecord {
  uint64_t id;
  uint32_t date;
  char title[49];
};

struct __attribute__((packed)) DeviceConfig {
  uint32_t magic;
  uint16_t version;
  uint16_t bytes;
  uint64_t sequence;
  char ssid[33];
  char password[64];
  int16_t timezoneMinutes;
  uint8_t use24Hour;
  uint8_t calendarFirstDay;
  uint8_t dateFormat;
  uint8_t alarmCount;
  uint8_t eventCount;
  uint64_t nextId;
  AlarmRecord alarms[MAX_ALARMS];
  EventRecord events[MAX_EVENTS];
  uint32_t crc;
};

static DeviceConfig config;
static SemaphoreHandle_t stateMutex = nullptr;
static Preferences preferences;
static AsyncWebServer server(80);
static DNSServer dnsServer;
static RTC_DS3231 rtc;
static bool rtcAvailable = false;
static bool timeValid = false;
static bool rtcSyncPending = false;
static bool configDirty = false;
static bool configForceWrite = false;
static bool configEverWrittenThisBoot = false;
static bool restartWhenSaved = false;
static bool alarmRinging = false;
static bool alarmOutputState = false;
static uint16_t ringingAlarmMask = 0;
static int64_t snoozeUntilEpoch = 0;
static int64_t lastRtcRefreshEpoch = 0;
static uint64_t lastFiredOccurrence[MAX_ALARMS] = {};
static uint64_t changeGeneration = 0;
static uint64_t storedSequence = 0;
static uint32_t dirtyAtMs = 0;
static uint32_t lastWriteAtMs = 0;
static uint32_t restartAtMs = 0;
static uint32_t nextRtcProbeMs = 0;
static uint32_t resetPressAtMs = 0;
static uint32_t lastSchedulerMs = 0;
static uint32_t lastRtcRefreshCheckMs = 0;
static bool resetButtonTiming = false;
static bool resetLatchedUntilRelease = false;

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"><meta name="theme-color" content="#10172b"><title>Alarm Clock</title>
<style>
:root{color-scheme:dark;--bg:#0b1020;--panel:#151d33;--panel2:#1b2741;--line:#2d3b5a;--text:#f2f5ff;--muted:#a4b0cc;--accent:#77e0ca;--accent2:#7c9cff;--danger:#ff7582;--shadow:0 18px 45px #03071377;font-family:Inter,ui-sans-serif,system-ui,-apple-system,"Segoe UI",sans-serif}*{box-sizing:border-box}body{margin:0;background:radial-gradient(1000px 500px at 85% -10%,#223969 0%,transparent 65%),var(--bg);color:var(--text);min-height:100vh}button,input,select{font:inherit}button{cursor:pointer}.shell{max-width:1180px;margin:auto;padding:20px clamp(14px,3vw,34px) 46px}.top{display:flex;align-items:center;justify-content:space-between;gap:18px;padding:12px 0 24px}.brand{display:flex;align-items:center;gap:12px;font-size:18px;font-weight:750;letter-spacing:.01em}.logo{width:42px;height:42px;border-radius:15px;background:linear-gradient(135deg,var(--accent),#6ea5ff);color:#10172b;display:grid;place-items:center;font-weight:900;font-size:19px;box-shadow:0 8px 25px #6ea5ff33}.status{display:flex;align-items:center;gap:8px;color:var(--muted);font-size:13px}.dot{width:9px;height:9px;border-radius:50%;background:var(--accent);box-shadow:0 0 0 4px #77e0ca19}.nav{display:flex;gap:8px;overflow:auto;padding:4px 2px 18px;scrollbar-width:none}.nav button{border:1px solid transparent;color:var(--muted);background:transparent;padding:10px 15px;border-radius:12px;white-space:nowrap}.nav button.active{background:var(--panel2);border-color:var(--line);color:var(--text)}main{min-height:60vh}.view{display:none}.view.active{display:block}.grid{display:grid;grid-template-columns:repeat(12,minmax(0,1fr));gap:16px}.card{background:linear-gradient(145deg,#17213a,#121a2d);border:1px solid var(--line);border-radius:20px;padding:clamp(17px,2.5vw,25px);box-shadow:var(--shadow);min-width:0}.span8{grid-column:span 8}.span4{grid-column:span 4}.span6{grid-column:span 6}.span12{grid-column:span 12}.eyebrow{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.12em;font-weight:700}.clock{font-size:clamp(48px,10vw,94px);line-height:1.04;letter-spacing:-.065em;font-variant-numeric:tabular-nums;font-weight:750;margin:12px 0 6px}.dateLine{font-size:17px;color:#c5cee3}.hero{min-height:230px;display:flex;flex-direction:column;justify-content:center;position:relative;overflow:hidden}.hero:after{content:"";position:absolute;right:-70px;bottom:-150px;width:350px;height:350px;border-radius:50%;border:1px solid #ffffff0b;box-shadow:0 0 0 36px #ffffff05,0 0 0 72px #ffffff03;pointer-events:none}.badge{display:inline-flex;align-items:center;gap:8px;background:#77e0ca18;color:var(--accent);border:1px solid #77e0ca38;border-radius:999px;padding:7px 11px;font-size:12px;font-weight:700;width:max-content}.alarmBanner{display:none;margin:0 0 16px;padding:18px;border-radius:18px;border:1px solid #ff758277;background:#60293666;align-items:center;justify-content:space-between;gap:12px}.alarmBanner.show{display:flex}.bannerTitle{font-weight:800;font-size:18px}.actions{display:flex;gap:9px;flex-wrap:wrap}.btn{border:1px solid var(--line);background:#243250;color:var(--text);border-radius:11px;padding:10px 14px;font-weight:700;transition:.15s transform,.15s background}.btn:hover{transform:translateY(-1px);background:#304165}.btn:disabled{opacity:.45;cursor:not-allowed;transform:none}.btn.primary{background:var(--accent);color:#09201d;border-color:transparent}.btn.danger{background:var(--danger);color:#2b0710;border-color:transparent}.btn.small{padding:7px 10px;font-size:13px}.btn.ghost{background:transparent}.stack{display:flex;flex-direction:column;gap:11px}.row{display:flex;align-items:center;justify-content:space-between;gap:14px}.row.wrap{flex-wrap:wrap}.muted{color:var(--muted)}.large{font-size:24px;font-weight:750}.sectionHead{display:flex;justify-content:space-between;align-items:flex-start;gap:14px;margin:0 0 17px}.sectionHead h1{font-size:clamp(25px,4vw,34px);letter-spacing:-.04em;margin:0 0 6px}.sectionHead p{margin:0;color:var(--muted)}.formGrid{display:grid;grid-template-columns:repeat(12,minmax(0,1fr));gap:13px}.field{grid-column:span 6;display:flex;flex-direction:column;gap:7px}.field.wide{grid-column:span 12}.field.third{grid-column:span 4}label{font-size:13px;color:#c8d1e5;font-weight:650}input,select{width:100%;background:#0d1527;color:var(--text);border:1px solid var(--line);border-radius:10px;padding:11px 12px;outline:none}input:focus,select:focus{border-color:var(--accent2);box-shadow:0 0 0 3px #7c9cff20}.checks{display:flex;gap:8px;flex-wrap:wrap}.day{display:flex;gap:6px;align-items:center;background:#0d1527;border:1px solid var(--line);padding:8px 9px;border-radius:10px;font-size:12px;color:var(--muted)}.day input{width:auto;accent-color:var(--accent)}.list{display:flex;flex-direction:column;gap:10px}.item{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:13px 14px;border:1px solid var(--line);background:#10182b;border-radius:14px}.itemMain{min-width:0}.itemTitle{font-weight:750;overflow-wrap:anywhere}.itemMeta{font-size:13px;color:var(--muted);margin-top:4px}.itemActions{display:flex;gap:7px;flex-shrink:0}.toggle{width:43px;height:25px;position:relative;display:inline-block}.toggle input{opacity:0;width:0;height:0}.slider{position:absolute;inset:0;background:#39445d;border-radius:99px;transition:.2s}.slider:before{content:"";position:absolute;width:19px;height:19px;left:3px;top:3px;background:#fff;border-radius:50%;transition:.2s}.toggle input:checked+.slider{background:#34a88f}.toggle input:checked+.slider:before{transform:translateX(18px)}.notice{padding:13px 15px;border-radius:12px;border:1px solid #7c9cff44;background:#7c9cff12;color:#d5def7;font-size:13px;line-height:1.55}.calendarHead{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:15px}.monthTitle{font-size:21px;font-weight:800}.calendarGrid{display:grid;grid-template-columns:repeat(7,minmax(0,1fr));gap:6px}.weekday,.dateCell{min-height:40px;display:grid;place-items:center}.weekday{font-size:11px;text-transform:uppercase;color:var(--muted);font-weight:700}.dateCell{border:1px solid transparent;background:#10182b;border-radius:10px;color:var(--text);position:relative}.dateCell.out{opacity:.36}.dateCell.today{border-color:var(--accent2)}.dateCell.selected{background:#77e0ca;color:#07201c;font-weight:800}.dateCell.hasEvent:after{content:"";width:4px;height:4px;background:var(--accent2);border-radius:50%;position:absolute;bottom:4px}.dateCell.selected.hasEvent:after{background:#142b57}.empty{padding:23px;text-align:center;border:1px dashed var(--line);border-radius:14px;color:var(--muted)}.toast{position:fixed;left:50%;bottom:20px;transform:translate(-50%,20px);background:#f1f5ff;color:#10172b;padding:12px 16px;border-radius:12px;box-shadow:var(--shadow);opacity:0;pointer-events:none;transition:.2s;z-index:10;font-weight:700;max-width:90vw}.toast.show{opacity:1;transform:translate(-50%,0)}.footer{margin-top:22px;color:#7887a8;font-size:12px;text-align:center}.switchline{display:flex;align-items:center;justify-content:space-between;gap:15px;padding:13px 0;border-bottom:1px solid #ffffff10}.hidden{display:none!important}
@media(max-width:760px){.span8,.span4,.span6{grid-column:span 12}.top{padding-bottom:16px}.clock{font-size:clamp(54px,17vw,85px)}.hero{min-height:195px}.item{align-items:flex-start}.itemActions{flex-wrap:wrap;justify-content:flex-end}.calendarGrid{gap:4px}.dateCell{min-height:38px}.alarmBanner{align-items:flex-start;flex-direction:column}.field,.field.third{grid-column:span 12}}
@media(min-width:761px) and (max-width:980px){.span8{grid-column:span 7}.span4{grid-column:span 5}}
</style></head>
<body><div class="shell"><header class="top"><div class="brand"><div class="logo">◷</div><div>ESP32 Alarm Clock<div class="muted" style="font-size:12px;font-weight:500;margin-top:2px">Local clock · alarms · calendar</div></div></div><div class="status"><span class="dot"></span><span id="connection">Connecting…</span></div></header>
<nav class="nav" aria-label="Main navigation"><button class="active" data-view="home">Overview</button><button data-view="alarms">Alarms</button><button data-view="time">Time settings</button><button data-view="calendar">Calendar</button><button data-view="wifi">Wi-Fi AP</button></nav>
<main>
<section class="view active" id="view-home"><div id="alarmBanner" class="alarmBanner"><div><div class="bannerTitle">Alarm is ringing</div><div class="muted" id="ringingNames">—</div><div class="muted">Dismiss it or snooze for five minutes.</div></div><div class="actions"><button class="btn" onclick="alarmAction('snooze')">Snooze 5 min</button><button class="btn danger" onclick="alarmAction('dismiss')">Dismiss</button></div></div><div class="grid"><div class="card hero span8"><div class="badge"><span class="dot"></span><span id="clockSource">Clock ready</span></div><div class="clock" id="clockDisplay">--:--</div><div class="dateLine" id="dateDisplay">Waiting for clock time…</div></div><div class="card span4 stack"><div class="eyebrow">Next alarm</div><div class="large" id="nextAlarm">No upcoming alarm</div><div class="muted" id="nextAlarmMeta">Create an alarm schedule to get started.</div><div class="actions"><button class="btn primary" onclick="showView('alarms')">Manage alarms</button></div></div><div class="card span6 stack"><div class="row"><div><div class="eyebrow">Today</div><div class="large" id="todaySummary">—</div></div><div class="badge" id="rtcBadge">Checking RTC</div></div><div class="muted" id="todayEvents">No calendar events today.</div></div><div class="card span6 stack"><div class="eyebrow">Device</div><div class="row"><span class="muted">Access point</span><strong id="homeSsid">—</strong></div><div class="row"><span class="muted">Time zone</span><strong id="homeTz">—</strong></div><div class="row"><span class="muted">Alarm output</span><strong>GPIO 25 · active high</strong></div></div></div></section>
<section class="view" id="view-alarms"><div class="sectionHead"><div><h1>Alarm schedules</h1><p>Manage up to 12 weekly or one-time schedules. Coincident alarms ring together.</p></div><button id="newAlarmButton" class="btn primary" onclick="resetAlarmForm()">＋ New alarm</button></div><div class="grid"><div class="card span8"><div class="list" id="alarmList"></div></div><div class="card span4"><div class="eyebrow" id="alarmFormHeading">New alarm</div><form id="alarmForm" class="stack" style="margin-top:13px" onsubmit="saveAlarm(event)"><input type="hidden" id="alarmId"><div class="field wide"><label for="alarmTitle">Label</label><input id="alarmTitle" maxlength="32" required placeholder="Wake up"></div><div class="formGrid"><div class="field"><label for="alarmTime">Time</label><input id="alarmTime" type="time" required value="07:00"></div><div class="field"><label for="onceDate">One-time date <span class="muted">(optional)</span></label><input id="onceDate" type="date"></div></div><div class="field wide"><label>Repeat on</label><div class="checks" id="weekdayChecks"></div></div><div class="switchline"><span>Alarm enabled</span><label class="toggle"><input id="alarmEnabled" type="checkbox" checked><span class="slider"></span></label></div><div class="actions"><button class="btn primary" type="submit">Save alarm</button><button class="btn ghost" type="button" onclick="resetAlarmForm()">Clear</button></div><div class="notice">For a one-time alarm, choose a date and leave repeat days unchecked. Weekly schedules use the device's selected time zone.</div></form></div></div></section>
<section class="view" id="view-time"><div class="sectionHead"><div><h1>Time settings</h1><p>Set the clock from this browser and select display preferences.</p></div></div><div class="grid"><div class="card span6 stack"><div class="eyebrow">Clock source</div><div class="row"><strong id="timeSource">Checking…</strong><span class="badge" id="timeValidBadge">—</span></div><div class="muted">The DS3231 is read at startup and kept synchronized. If it is unavailable, the ESP32 system clock is used until power is lost.</div><button class="btn primary" onclick="syncBrowserTime()">Sync from this device</button><div class="notice">The browser sends its current UTC timestamp and time-zone offset. Keep this page open until the sync confirmation appears.</div></div><div class="card span6"><div class="eyebrow">Local time preferences</div><form id="timeForm" class="stack" style="margin-top:13px" onsubmit="saveTimeSettings(event)"><div class="field wide"><label for="timezone">UTC offset in minutes</label><input id="timezone" type="number" min="-840" max="840" step="15" required></div><div class="switchline"><span>Use 24-hour display</span><label class="toggle"><input id="format24" type="checkbox"><span class="slider"></span></label></div><div class="actions"><button class="btn primary" type="submit">Save time preferences</button></div><div class="muted">Offset range: UTC−14:00 through UTC+14:00. Daylight-saving changes can be applied by changing the offset here or syncing again.</div></form></div></div></section>
<section class="view" id="view-calendar"><div class="sectionHead"><div><h1>Calendar</h1><p>Browse dates and manage local calendar entries.</p></div></div><div class="grid"><div class="card span8"><div class="calendarHead"><button class="btn small" onclick="shiftMonth(-1)" aria-label="Previous month">‹</button><div class="monthTitle" id="monthTitle">—</div><button class="btn small" onclick="shiftMonth(1)" aria-label="Next month">›</button></div><div class="calendarGrid" id="calendarGrid"></div></div><div class="card span4 stack"><div><div class="eyebrow">Selected date</div><div class="large" id="selectedDateLabel">—</div></div><form class="stack" onsubmit="saveEvent(event)"><input type="hidden" id="eventId"><div class="field wide"><label for="eventTitle">Event title</label><input id="eventTitle" maxlength="48" placeholder="Appointment" required></div><div class="actions"><button class="btn primary" type="submit">Save event</button><button class="btn ghost" type="button" onclick="resetEventForm()">Clear</button></div></form><div class="eyebrow">Events on this date</div><div class="list" id="eventList"></div></div><div class="card span12"><div class="eyebrow">Calendar settings</div><form class="formGrid" style="margin-top:12px" onsubmit="saveCalendarSettings(event)"><div class="field"><label for="weekStart">First day of week</label><select id="weekStart"><option value="0">Sunday</option><option value="1">Monday</option></select></div><div class="field"><label for="dateFormat">Date format</label><select id="dateFormat"><option value="0">MM/DD/YYYY</option><option value="1">DD/MM/YYYY</option><option value="2">YYYY-MM-DD</option></select></div><div class="field wide"><div class="row wrap"><span class="muted">Calendar entries are stored locally and follow the clock's selected UTC offset.</span><button class="btn primary" type="submit">Save calendar settings</button></div></div></form></div></div></section>
<section class="view" id="view-wifi"><div class="sectionHead"><div><h1>Wi-Fi access point</h1><p>Manage the clock's private setup network and captive portal.</p></div></div><div class="grid"><div class="card span6 stack"><div class="eyebrow">Current network</div><div class="row"><span class="muted">SSID</span><strong id="wifiCurrentSsid">—</strong></div><div class="row"><span class="muted">Portal address</span><strong>http://192.168.4.1</strong></div><div class="row"><span class="muted">Captive portal</span><strong>Enabled</strong></div><div class="notice">Connect to this access point, then open any web page. DNS requests are directed to 192.168.4.1.</div></div><div class="card span6"><div class="eyebrow">Network settings</div><form class="stack" style="margin-top:13px" onsubmit="saveWifi(event)"><div class="field wide"><label for="ssid">Network name (SSID)</label><input id="ssid" maxlength="32" required autocomplete="off"></div><div class="field wide"><label for="wifiPassword">New password</label><input id="wifiPassword" type="password" minlength="8" maxlength="63" autocomplete="new-password" placeholder="Leave blank to keep current"></div><div class="notice">The default network is ESP32_ALARM_CLOCK with password 12345678. Set a private password. Saving network settings restarts the access point; reconnect using the new details.</div><button class="btn primary" type="submit">Save and restart access point</button></form></div></div></section>
</main><div class="footer">ESP32 local-only clock · No cloud account required</div></div><div class="toast" id="toast" role="status"></div>
<script>
const DAYS=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];let state=null;let selectedDate=new Date();selectedDate.setHours(12,0,0,0);let viewYear=selectedDate.getFullYear(),viewMonth=selectedDate.getMonth();let selectedKey='';let toastTimer=0;let loading=false;
const $=id=>document.getElementById(id);const esc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
function toast(text){const t=$('toast');t.textContent=text;t.classList.add('show');clearTimeout(toastTimer);toastTimer=setTimeout(()=>t.classList.remove('show'),2800)}
async function api(path,method='GET',data=null,retries=2){const options={method,cache:'no-store'};if(data){options.headers={'Content-Type':'application/x-www-form-urlencoded'};options.body=new URLSearchParams(data)}const r=await fetch(path,options);let body={};try{body=await r.json()}catch{}if(r.status===503&&retries>0){await new Promise(resolve=>setTimeout(resolve,100));return api(path,method,data,retries-1)}if(!r.ok)throw Error(body.error||`Request failed (${r.status})`);return body}
function showView(name){document.querySelectorAll('.view').forEach(v=>v.classList.toggle('active',v.id==='view-'+name));document.querySelectorAll('.nav button').forEach(b=>b.classList.toggle('active',b.dataset.view===name));if(name==='calendar')renderCalendar()}
document.querySelectorAll('.nav button').forEach(b=>b.addEventListener('click',()=>showView(b.dataset.view)));
function z2(v){return String(v).padStart(2,'0')}function keyDate(y,m,d){return `${y}${z2(m+1)}${z2(d)}`}function keyFromLocal(d){return keyDate(d.getUTCFullYear(),d.getUTCMonth(),d.getUTCDate())}
function localParts(){if(!state)return new Date();return new Date((Number(state.now)+Number(state.tz)*60)*1000)}
function formatOffset(n){const sign=n>=0?'+':'−';const a=Math.abs(n);return `UTC${sign}${z2(Math.floor(a/60))}:${z2(a%60)}`}
function formatDate(y,m,d){if(state?.dateFormat===1)return `${z2(d)}/${z2(m+1)}/${y}`;if(state?.dateFormat===2)return `${y}-${z2(m+1)}-${z2(d)}`;return `${z2(m+1)}/${z2(d)}/${y}`}
function formatDateKey(value){const s=String(value);return formatDate(Number(s.slice(0,4)),Number(s.slice(4,6))-1,Number(s.slice(6,8)))}
function updateClock(){if(!state||!state.valid){$('clockDisplay').textContent='--:--';$('dateDisplay').textContent='Clock needs sync';return}const d=localParts();let h=d.getUTCHours(),m=d.getUTCMinutes();let time=state.use24?`${z2(h)}:${z2(m)}`:`${h%12||12}:${z2(m)} ${h>=12?'PM':'AM'}`;$('clockDisplay').textContent=time;$('dateDisplay').textContent=`${DAYS[d.getUTCDay()]}, ${formatDate(d.getUTCFullYear(),d.getUTCMonth(),d.getUTCDate())} · ${formatOffset(state.tz)}`}
function dateAt(epoch){return new Date((Number(epoch)+Number(state.tz)*60)*1000)}
function renderDashboard(){if(!state)return;updateClock();$('connection').textContent='Connected · 192.168.4.1';$('clockSource').textContent=state.rtc?'DS3231 synchronized':'ESP32 internal clock';$('rtcBadge').textContent=state.rtc?'DS3231 available':'ESP32 clock fallback';$('rtcBadge').style.color=state.rtc?'var(--accent)':'#f2ca78';$('homeSsid').textContent=state.ssid;$('homeTz').textContent=formatOffset(state.tz);$('wifiCurrentSsid').textContent=state.ssid;$('timeSource').textContent=state.rtc?'DS3231 real-time clock':'ESP32 system clock';$('timeValidBadge').textContent=state.valid?'Time set':'Needs sync';$('timeValidBadge').style.color=state.valid?'var(--accent)':'var(--danger)';$('timezone').value=state.tz;$('format24').checked=!!state.use24;$('ssid').value=state.ssid;$('weekStart').value=String(state.calendarFirstDay);$('dateFormat').value=String(state.dateFormat);
$('alarmBanner').classList.toggle('show',!!state.ringing);$('ringingNames').textContent=(state.ringingAlarms||[]).join(' · ')||'Alarm schedule active';let nowLocal=localParts();$('todaySummary').textContent=`${DAYS[nowLocal.getUTCDay()]}, ${formatDate(nowLocal.getUTCFullYear(),nowLocal.getUTCMonth(),nowLocal.getUTCDate())}`;let today=keyDate(nowLocal.getUTCFullYear(),nowLocal.getUTCMonth(),nowLocal.getUTCDate());let todays=state.events.filter(e=>String(e.date)===today);$('todayEvents').textContent=todays.length?todays.map(e=>e.title).join(' · '):'No calendar events today.';renderAlarms();renderCalendar();}
function renderAlarms(){const root=$('alarmList');if(!root)return;$('newAlarmButton').disabled=state.alarms.length>=12;if(!state.alarms.length){root.innerHTML='<div class="empty">No alarms yet. Add a weekly or one-time schedule.</div>';$('nextAlarm').textContent='No upcoming alarm';$('nextAlarmMeta').textContent='Create an alarm schedule to get started.';return}root.innerHTML=state.alarms.map(a=>{let when=state.use24?`${z2(a.hour)}:${z2(a.minute)}`:`${a.hour%12||12}:${z2(a.minute)} ${a.hour>=12?'PM':'AM'}`;let schedule=a.once?`One time · ${formatDateKey(a.once)}`:(a.days?DAYS.filter((_,i)=>a.days&(1<<i)).join(', '):'No repeat days');return `<div class="item"><div class="itemMain"><div class="itemTitle">${esc(a.title)} <span class="muted">· ${when}</span></div><div class="itemMeta">${schedule} · ${a.enabled?'Enabled':'Disabled'}</div></div><div class="itemActions"><button class="btn small" onclick="editAlarm(${a.id})">Edit</button><button class="btn small danger" onclick="deleteAlarm(${a.id})">Delete</button></div></div>`}).join('');let nowLocal=localParts(),nowEpoch=Number(state.now),next=null;if(state.valid)for(const a of state.alarms){if(!a.enabled)continue;if(a.once){const k=String(a.once),candidate=Date.UTC(Number(k.slice(0,4)),Number(k.slice(4,6))-1,Number(k.slice(6,8)),a.hour,a.minute)/1000-Number(state.tz)*60;if(candidate>=nowEpoch&&(!next||candidate<next.epoch))next={alarm:a,epoch:candidate}}else{for(let offset=0;offset<=7;offset++){const d=new Date(Date.UTC(nowLocal.getUTCFullYear(),nowLocal.getUTCMonth(),nowLocal.getUTCDate()+offset,a.hour,a.minute));const candidate=d.getTime()/1000-Number(state.tz)*60;if((a.days&(1<<d.getUTCDay()))&&candidate>=nowEpoch&&(!next||candidate<next.epoch))next={alarm:a,epoch:candidate}}}}if(!next){$('nextAlarm').textContent=state.valid?'No upcoming alarm':'Clock needs sync';$('nextAlarmMeta').textContent=state.valid?'Enable a future alarm schedule to see its next time.':'Sync device time to calculate the next alarm.'}else{const a=next.alarm,d=dateAt(next.epoch),when=state.use24?`${z2(a.hour)}:${z2(a.minute)}`:`${a.hour%12||12}:${z2(a.minute)} ${a.hour>=12?'PM':'AM'}`;$('nextAlarm').textContent=`${a.title} · ${when}`;$('nextAlarmMeta').textContent=`${DAYS[d.getUTCDay()]}, ${formatDate(d.getUTCFullYear(),d.getUTCMonth(),d.getUTCDate())}`}}
function initWeekdays(){ $('weekdayChecks').innerHTML=DAYS.map((d,i)=>`<label class="day"><input type="checkbox" value="${i}">${d}</label>`).join('') }initWeekdays();
function resetAlarmForm(){ $('alarmId').value='';$('alarmTitle').value='';$('alarmTime').value='07:00';$('onceDate').value='';$('alarmEnabled').checked=true;document.querySelectorAll('#weekdayChecks input').forEach(x=>x.checked=false);$('alarmFormHeading').textContent='New alarm' }
function editAlarm(id){const a=state.alarms.find(x=>x.id===id);if(!a)return;$('alarmId').value=a.id;$('alarmTitle').value=a.title;$('alarmTime').value=`${z2(a.hour)}:${z2(a.minute)}`;$('onceDate').value=a.once?`${String(a.once).slice(0,4)}-${String(a.once).slice(4,6)}-${String(a.once).slice(6,8)}`:'';$('alarmEnabled').checked=!!a.enabled;document.querySelectorAll('#weekdayChecks input').forEach(x=>x.checked=!!(a.days&(1<<Number(x.value))));$('alarmFormHeading').textContent='Edit alarm';$('alarmTitle').focus();window.scrollTo({top:0,behavior:'smooth'})}
async function saveAlarm(e){e.preventDefault();const [hour,minute]=$('alarmTime').value.split(':').map(Number);let days=0;document.querySelectorAll('#weekdayChecks input:checked').forEach(x=>days|=1<<Number(x.value));const d=$('onceDate').value;const data={id:$('alarmId').value||'0',title:$('alarmTitle').value,hour,minute,days,once:d?d.replaceAll('-',''):'0',enabled:$('alarmEnabled').checked?'1':'0'};try{await api('/api/alarms','POST',data);await refresh();resetAlarmForm();toast('Alarm saved')}catch(err){toast(err.message)}}
async function deleteAlarm(id){if(!confirm('Delete this alarm?'))return;try{await api('/api/alarms?id='+encodeURIComponent(id),'DELETE');await refresh();toast('Alarm deleted')}catch(err){toast(err.message)}}
async function alarmAction(action){try{await api('/api/alarm/action','POST',{action,minutes:5});await refresh();toast(action==='snooze'?'Snoozed for five minutes':'Alarm dismissed')}catch(err){toast(err.message)}}
async function syncBrowserTime(){try{const tz=-new Date().getTimezoneOffset();await api('/api/time','POST',{epochMs:String(Date.now()),tz:String(tz)});if(state){state.now=Math.floor(Date.now()/1000);state.valid=true;state.tz=tz;renderDashboard()}toast('Clock synchronized from this device')}catch(err){toast(err.message)}}
async function saveTimeSettings(e){e.preventDefault();try{await api('/api/settings','POST',{tz:$('timezone').value,use24:$('format24').checked?'1':'0'});await refresh();toast('Time preferences saved')}catch(err){toast(err.message)}}
async function saveCalendarSettings(e){e.preventDefault();try{await api('/api/settings','POST',{firstDay:$('weekStart').value,dateFormat:$('dateFormat').value});await refresh();toast('Calendar settings saved')}catch(err){toast(err.message)}}
async function saveWifi(e){e.preventDefault();const data={ssid:$('ssid').value};if($('wifiPassword').value)data.password=$('wifiPassword').value;try{const result=await api('/api/settings','POST',data);if(result.restarting){toast('Saved. Access point is restarting…');setTimeout(()=>{location.href='http://192.168.4.1/'},8000)}else toast('No network changes were needed')}catch(err){toast(err.message)}}
function resetEventForm(){ $('eventId').value='';$('eventTitle').value=''}
async function saveEvent(e){e.preventDefault();if(!selectedKey)return;try{await api('/api/events','POST',{id:$('eventId').value||'0',date:selectedKey,title:$('eventTitle').value});await refresh();resetEventForm();toast('Calendar event saved')}catch(err){toast(err.message)}}
async function deleteEvent(id){if(!confirm('Delete this calendar event?'))return;try{await api('/api/events?id='+encodeURIComponent(id),'DELETE');await refresh();toast('Event deleted')}catch(err){toast(err.message)}}
function editEvent(id){const e=state.events.find(x=>x.id===id);if(!e)return;$('eventId').value=e.id;$('eventTitle').value=e.title;$('eventTitle').focus()}
function shiftMonth(n){viewMonth+=n;if(viewMonth<0){viewMonth=11;viewYear--}if(viewMonth>11){viewMonth=0;viewYear++}renderCalendar()}
function renderCalendar(){if(!state)return;const root=$('calendarGrid');if(!root)return;if(!selectedKey&&state.valid){const current=localParts();viewYear=current.getUTCFullYear();viewMonth=current.getUTCMonth()}const months=['January','February','March','April','May','June','July','August','September','October','November','December'];$('monthTitle').textContent=`${months[viewMonth]} ${viewYear}`;const labels=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];const firstDay=Number(state.calendarFirstDay)||0;root.innerHTML=Array.from({length:7},(_,i)=>`<div class="weekday">${labels[(i+firstDay)%7]}</div>`).join('');const first=new Date(Date.UTC(viewYear,viewMonth,1)).getUTCDay();const offset=(first-firstDay+7)%7;const total=new Date(Date.UTC(viewYear,viewMonth+1,0)).getUTCDate();const prevTotal=new Date(Date.UTC(viewYear,viewMonth,0)).getUTCDate();const today=state.valid?keyFromLocal(localParts()):'';for(let i=0;i<42;i++){let n=i-offset+1,y=viewYear,m=viewMonth,d=n,other=false;if(n<1){d=prevTotal+n;m=viewMonth-1;other=true;if(m<0){m=11;y--}}else if(n>total){d=n-total;m=viewMonth+1;other=true;if(m>11){m=0;y++}}const k=keyDate(y,m,d),has=state.events.some(e=>String(e.date)===k);root.insertAdjacentHTML('beforeend',`<button class="dateCell ${other?'out':''} ${k===today?'today':''} ${k===selectedKey?'selected':''} ${has?'hasEvent':''}" onclick="selectDate('${k}')">${d}</button>`)}if(!selectedKey)selectedKey=today||keyDate(viewYear,viewMonth,1);renderSelectedDate()}
function selectDate(k){selectedKey=k;const y=Number(k.slice(0,4)),m=Number(k.slice(4,6))-1,d=Number(k.slice(6,8));viewYear=y;viewMonth=m;renderCalendar()}
function renderSelectedDate(){if(!selectedKey)return;let y=Number(selectedKey.slice(0,4)),m=Number(selectedKey.slice(4,6))-1,d=Number(selectedKey.slice(6,8));$('selectedDateLabel').textContent=formatDate(y,m,d);const items=state.events.filter(e=>String(e.date)===selectedKey);$('eventList').innerHTML=items.length?items.map(e=>`<div class="item"><div class="itemMain"><div class="itemTitle">${esc(e.title)}</div></div><div class="itemActions"><button class="btn small" onclick="editEvent(${e.id})">Edit</button><button class="btn small danger" onclick="deleteEvent(${e.id})">Delete</button></div></div>`).join(''):'<div class="empty">No events for this date.</div>'}
async function refresh(){if(loading)return;loading=true;try{state=await api('/api/state');renderDashboard();if(!state.valid&&!sessionStorage.getItem('browserTimeSynced')){sessionStorage.setItem('browserTimeSynced','1');await syncBrowserTime()}}catch(err){$('connection').textContent='Not connected';}finally{loading=false}}
setInterval(()=>{if(state){state.now=Number(state.now)+1;updateClock()}},1000);setInterval(refresh,5000);refresh();
</script></body></html>)HTML";

static bool takeStateLock() {
  return stateMutex && xSemaphoreTake(stateMutex, 0) == pdTRUE;
}

static void giveStateLock() {
  if (stateMutex) xSemaphoreGive(stateMutex);
}

static uint32_t crc32Bytes(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
  }
  return ~crc;
}

static bool validDate(uint32_t value) {
  uint16_t year = value / 10000U;
  uint8_t month = (value / 100U) % 100U;
  uint8_t day = value % 100U;
  if (year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1) return false;
  static const uint8_t monthDays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  uint8_t maxDay = monthDays[month - 1];
  bool leap = (year % 4U == 0U && year % 100U != 0U) || year % 400U == 0U;
  if (month == 2 && leap) ++maxDay;
  return day <= maxDay;
}

static bool configValid(const DeviceConfig &candidate) {
  if (candidate.magic != CONFIG_MAGIC || candidate.version != CONFIG_VERSION || candidate.bytes != sizeof(DeviceConfig)) return false;
  if (candidate.alarmCount > MAX_ALARMS || candidate.eventCount > MAX_EVENTS) return false;
  if (candidate.timezoneMinutes < -840 || candidate.timezoneMinutes > 840 || candidate.use24Hour > 1 || candidate.calendarFirstDay > 1 || candidate.dateFormat > 2) return false;
  if (candidate.ssid[0] == 0 || memchr(candidate.ssid, 0, sizeof(candidate.ssid)) == nullptr || memchr(candidate.password, 0, sizeof(candidate.password)) == nullptr) return false;
  if (strlen(candidate.password) < 8 || strlen(candidate.password) > 63) return false;
  if (crc32Bytes(reinterpret_cast<const uint8_t *>(&candidate), offsetof(DeviceConfig, crc)) != candidate.crc) return false;
  for (uint8_t i = 0; i < candidate.alarmCount; ++i) {
    const AlarmRecord &a = candidate.alarms[i];
    if (a.hour > 23 || a.minute > 59 || a.weekdays > 127 || memchr(a.title, 0, sizeof(a.title)) == nullptr) return false;
    if (a.onceDate && !validDate(a.onceDate)) return false;
  }
  for (uint8_t i = 0; i < candidate.eventCount; ++i) {
    const EventRecord &e = candidate.events[i];
    if (!validDate(e.date) || memchr(e.title, 0, sizeof(e.title)) == nullptr) return false;
  }
  return true;
}

static void setDefaults(DeviceConfig &target) {
  memset(&target, 0, sizeof(target));
  target.magic = CONFIG_MAGIC;
  target.version = CONFIG_VERSION;
  target.bytes = sizeof(DeviceConfig);
  memcpy(target.ssid, "ESP32_ALARM_CLOCK", sizeof("ESP32_ALARM_CLOCK"));
  memcpy(target.password, "12345678", sizeof("12345678"));
  target.timezoneMinutes = 480;
  target.use24Hour = 0;
  target.calendarFirstDay = 0;
  target.dateFormat = 0;
  target.nextId = 1;
  target.crc = crc32Bytes(reinterpret_cast<const uint8_t *>(&target), offsetof(DeviceConfig, crc));
}

static bool readSlot(const char *key, DeviceConfig &out) {
  size_t bytes = preferences.getBytesLength(key);
  if (bytes != sizeof(DeviceConfig)) return false;
  if (preferences.getBytes(key, &out, sizeof(DeviceConfig)) != sizeof(DeviceConfig)) return false;
  return configValid(out);
}

static void loadConfiguration() {
  DeviceConfig first{};
  DeviceConfig second{};
  bool validFirst = readSlot("slot0", first);
  bool validSecond = readSlot("slot1", second);
  if (validFirst && validSecond) {
    config = static_cast<int64_t>(first.sequence - second.sequence) > 0 ? first : second;
  } else if (validFirst) {
    config = first;
  } else if (validSecond) {
    config = second;
  } else {
    setDefaults(config);
    configDirty = true;
    dirtyAtMs = millis();
    changeGeneration++;
  }
  storedSequence = config.sequence;
}

static void markConfigDirty(bool forceWrite = false) {
  ++changeGeneration;
  if (!configDirty) dirtyAtMs = millis();
  configDirty = true;
  if (forceWrite) configForceWrite = true;
}

static bool timeIsUsable(int64_t epoch) {
  return epoch >= MIN_VALID_EPOCH && epoch <= MAX_VALID_EPOCH;
}

static int64_t currentEpoch() {
  return static_cast<int64_t>(time(nullptr));
}

static void setSystemEpoch(int64_t epoch) {
  struct timeval tv;
  tv.tv_sec = static_cast<time_t>(epoch);
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
}

static void streamJsonString(Print &out, const char *text) {
  out.write('"');
  if (text) {
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
      unsigned char c = *p;
      if (c == '"' || c == '\\') { out.write('\\'); out.write(c); }
      else if (c < 0x20) {
        static const char hex[] = "0123456789abcdef";
        out.print("\\u00"); out.write(hex[c >> 4]); out.write(hex[c & 0x0F]);
      } else out.write(c);
    }
  }
  out.write('"');
}

static void streamInt64(Print &out, int64_t value) {
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
  out.print(buffer);
}

static AsyncResponseStream *jsonResponse(AsyncWebServerRequest *request, int status = 200) {
  AsyncResponseStream *response = request->beginResponseStream("application/json", 6144);
  response->setCode(status);
  response->addHeader("Cache-Control", "no-store");
  response->addHeader("X-Content-Type-Options", "nosniff");
  return response;
}

static String getArgument(AsyncWebServerRequest *request, const char *name, bool &found) {
  if (request->hasParam(name, true)) { found = true; return request->getParam(name, true)->value(); }
  if (request->hasParam(name, false)) { found = true; return request->getParam(name, false)->value(); }
  found = false;
  return String();
}

static bool parseIntArgument(AsyncWebServerRequest *request, const char *name, int64_t minimum, int64_t maximum, int64_t &value, bool required = true) {
  bool found = false;
  String text = getArgument(request, name, found);
  if (!found) return !required;
  if (text.length() == 0 || text.length() > 20) return false;
  char *end = nullptr;
  long long parsed = strtoll(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != 0 || parsed < minimum || parsed > maximum) return false;
  value = static_cast<int64_t>(parsed);
  return true;
}

static bool copyTextArgument(AsyncWebServerRequest *request, const char *name, char *destination, size_t capacity, bool required = true) {
  bool found = false;
  String text = getArgument(request, name, found);
  if (!found) return !required;
  if (text.length() == 0 || text.length() >= capacity) return false;
  for (size_t i = 0; i < text.length(); ++i) {
    uint8_t c = static_cast<uint8_t>(text[i]);
    if (c < 0x20 || c == 0x7F) return false;
  }
  memset(destination, 0, capacity);
  memcpy(destination, text.c_str(), text.length());
  return true;
}

static void sendError(AsyncWebServerRequest *request, int code, const char *message) {
  AsyncResponseStream *response = jsonResponse(request, code);
  response->print("{\"error\":");
  streamJsonString(*response, message);
  response->print("}");
  request->send(response);
}

static void handleState(AsyncWebServerRequest *request) {
  DeviceConfig snapshot{};
  bool valid = false, rtcPresent = false, ringing = false;
  uint16_t ringingMask = 0;
  int64_t snooze = 0;
  if (!takeStateLock()) { sendError(request, 503, "Device busy; retry shortly"); return; }
  snapshot = config;
  valid = timeValid;
  rtcPresent = rtcAvailable;
  ringing = alarmRinging;
  ringingMask = ringingAlarmMask;
  snooze = snoozeUntilEpoch;
  giveStateLock();
  AsyncResponseStream *response = jsonResponse(request);
  int64_t now = currentEpoch();
  response->print("{\"now\":"); streamInt64(*response, now);
  response->print(",\"valid\":"); response->print(valid && timeIsUsable(now) ? "true" : "false");
  response->print(",\"rtc\":"); response->print(rtcPresent ? "true" : "false");
  response->print(",\"ringing\":"); response->print(ringing ? "true" : "false");
  response->print(",\"ringingAlarms\":[");
  bool firstRingingName = true;
  for (uint8_t i = 0; i < snapshot.alarmCount; ++i) if (ringingMask & (1U << i)) {
    if (!firstRingingName) response->write(',');
    streamJsonString(*response, snapshot.alarms[i].title);
    firstRingingName = false;
  }
  response->print(']');
  response->print(",\"snoozeUntil\":"); streamInt64(*response, snooze);
  response->print(",\"tz\":"); response->print(snapshot.timezoneMinutes);
  response->print(",\"use24\":"); response->print(snapshot.use24Hour ? "true" : "false");
  response->print(",\"calendarFirstDay\":"); response->print(snapshot.calendarFirstDay);
  response->print(",\"dateFormat\":"); response->print(snapshot.dateFormat);
  response->print(",\"ssid\":"); streamJsonString(*response, snapshot.ssid);
  response->print(",\"alarms\":[");
  for (uint8_t i = 0; i < snapshot.alarmCount; ++i) {
    if (i) response->write(',');
    const AlarmRecord &a = snapshot.alarms[i];
    response->print("{\"id\":"); streamInt64(*response, static_cast<int64_t>(a.id));
    response->print(",\"title\":"); streamJsonString(*response, a.title);
    response->print(",\"enabled\":"); response->print(a.enabled ? "true" : "false");
    response->print(",\"hour\":"); response->print(a.hour);
    response->print(",\"minute\":"); response->print(a.minute);
    response->print(",\"days\":"); response->print(a.weekdays);
    response->print(",\"once\":"); response->print(a.onceDate);
    response->print('}');
  }
  response->print("],\"events\":[");
  for (uint8_t i = 0; i < snapshot.eventCount; ++i) {
    if (i) response->write(',');
    const EventRecord &e = snapshot.events[i];
    response->print("{\"id\":"); streamInt64(*response, static_cast<int64_t>(e.id));
    response->print(",\"date\":"); response->print(e.date);
    response->print(",\"title\":"); streamJsonString(*response, e.title);
    response->print('}');
  }
  response->print("]}");
  request->send(response);
}

static void handleSetTime(AsyncWebServerRequest *request) {
  int64_t epochMs = 0, timezone = 0;
  if (!parseIntArgument(request, "epochMs", MIN_VALID_EPOCH * 1000LL, MAX_VALID_EPOCH * 1000LL, epochMs) ||
      !parseIntArgument(request, "tz", -840, 840, timezone)) { sendError(request, 400, "Invalid browser time or UTC offset"); return; }
  int64_t epoch = epochMs / 1000LL;
  if (!timeIsUsable(epoch)) { sendError(request, 400, "Time is outside the supported range"); return; }
  if (!takeStateLock()) { sendError(request, 503, "Device busy; retry shortly"); return; }
  setSystemEpoch(epoch);
  timeValid = true;
  if (config.timezoneMinutes != timezone) { config.timezoneMinutes = static_cast<int16_t>(timezone); markConfigDirty(); }
  rtcSyncPending = true;
  lastRtcRefreshEpoch = epoch;
  giveStateLock();
  request->send(202, "application/json", "{\"ok\":true}");
}

static void handleSettings(AsyncWebServerRequest *request) {
  bool hasTz = false, hasFormat = false, hasSsid = false, hasPassword = false;
  bool hasFirstDay = false, hasDateFormat = false;
  String tzText = getArgument(request, "tz", hasTz);
  String formatText = getArgument(request, "use24", hasFormat);
  String ssidText = getArgument(request, "ssid", hasSsid);
  String passwordText = getArgument(request, "password", hasPassword);
  String firstDayText = getArgument(request, "firstDay", hasFirstDay);
  String dateFormatText = getArgument(request, "dateFormat", hasDateFormat);
  int64_t timezone = 0;
  if (hasTz) {
    char *end = nullptr; long parsed = strtol(tzText.c_str(), &end, 10);
    if (!tzText.length() || *end || parsed < -840 || parsed > 840) { sendError(request, 400, "UTC offset must be between -840 and 840 minutes"); return; }
    timezone = parsed;
  }
  if (hasFormat && formatText != "0" && formatText != "1") { sendError(request, 400, "Invalid clock display preference"); return; }
  if (hasFirstDay && firstDayText != "0" && firstDayText != "1") { sendError(request, 400, "Week start must be Sunday or Monday"); return; }
  if (hasDateFormat && dateFormatText != "0" && dateFormatText != "1" && dateFormatText != "2") { sendError(request, 400, "Invalid date format"); return; }
  if (hasSsid && (ssidText.length() == 0 || ssidText.length() > 32)) { sendError(request, 400, "SSID must contain 1 to 32 characters"); return; }
  if (hasPassword && passwordText.length() && (passwordText.length() < 8 || passwordText.length() > 63)) { sendError(request, 400, "Wi-Fi password must contain 8 to 63 characters"); return; }
  if (hasSsid) for (size_t i = 0; i < ssidText.length(); ++i) { uint8_t c = static_cast<uint8_t>(ssidText[i]); if (c < 0x20 || c == 0x7F) { sendError(request, 400, "SSID cannot contain control characters"); return; } }
  if (hasPassword) for (size_t i = 0; i < passwordText.length(); ++i) { uint8_t c = static_cast<uint8_t>(passwordText[i]); if (c < 0x20 || c == 0x7F) { sendError(request, 400, "Password cannot contain control characters"); return; } }
  if (!hasTz && !hasFormat && !hasFirstDay && !hasDateFormat && !hasSsid && !hasPassword) { sendError(request, 400, "No settings to update"); return; }
  if (!takeStateLock()) { sendError(request, 503, "Device busy; retry shortly"); return; }
  bool wifiChanged = false, configChanged = false;
  if (hasTz && config.timezoneMinutes != timezone) { config.timezoneMinutes = static_cast<int16_t>(timezone); configChanged = true; }
  if (hasFormat && config.use24Hour != (formatText == "1" ? 1 : 0)) { config.use24Hour = formatText == "1" ? 1 : 0; configChanged = true; }
  if (hasFirstDay && config.calendarFirstDay != (firstDayText == "1" ? 1 : 0)) { config.calendarFirstDay = firstDayText == "1" ? 1 : 0; configChanged = true; }
  if (hasDateFormat && config.dateFormat != dateFormatText.toInt()) { config.dateFormat = static_cast<uint8_t>(dateFormatText.toInt()); configChanged = true; }
  if (hasSsid && (strlen(config.ssid) != ssidText.length() || memcmp(config.ssid, ssidText.c_str(), ssidText.length()) != 0)) {
    memset(config.ssid, 0, sizeof(config.ssid)); memcpy(config.ssid, ssidText.c_str(), ssidText.length()); wifiChanged = true; configChanged = true;
  }
  if (hasPassword && passwordText.length() && (strlen(config.password) != passwordText.length() || memcmp(config.password, passwordText.c_str(), passwordText.length()) != 0)) {
    memset(config.password, 0, sizeof(config.password)); memcpy(config.password, passwordText.c_str(), passwordText.length()); wifiChanged = true; configChanged = true;
  }
  if (configChanged) markConfigDirty(wifiChanged);
  if (wifiChanged) { restartWhenSaved = true; restartAtMs = millis() + 2000UL; }
  giveStateLock();
  request->send(wifiChanged ? 202 : 200, "application/json", wifiChanged ? "{\"ok\":true,\"restarting\":true}" : "{\"ok\":true}");
}

static void handleSaveAlarm(AsyncWebServerRequest *request) {
  int64_t id = 0, hour = 0, minute = 0, days = 0, once = 0, enabled = 1;
  bool idPresent = false;
  String idText = getArgument(request, "id", idPresent);
  if (idPresent) { char *end = nullptr; long long parsed = strtoll(idText.c_str(), &end, 10); if (!idText.length() || *end || parsed < 0) { sendError(request, 400, "Invalid alarm id"); return; } id = parsed; }
  if (!parseIntArgument(request, "hour", 0, 23, hour) || !parseIntArgument(request, "minute", 0, 59, minute) ||
      !parseIntArgument(request, "days", 0, 127, days) || !parseIntArgument(request, "once", 0, 20991231, once) ||
      !parseIntArgument(request, "enabled", 0, 1, enabled)) { sendError(request, 400, "Invalid alarm schedule"); return; }
  if (once && !validDate(static_cast<uint32_t>(once))) { sendError(request, 400, "Invalid one-time alarm date"); return; }
  if (!once && days == 0) { sendError(request, 400, "Choose at least one repeat day or a one-time date"); return; }
  char title[33] = {};
  if (!copyTextArgument(request, "title", title, sizeof(title))) { sendError(request, 400, "Alarm label must contain 1 to 32 characters"); return; }
  if (!takeStateLock()) { sendError(request, 503, "Device busy; retry shortly"); return; }
  int index = -1;
  if (id) for (uint8_t i = 0; i < config.alarmCount; ++i) if (config.alarms[i].id == static_cast<uint64_t>(id)) { index = i; break; }
  if (!id && config.alarmCount < MAX_ALARMS) index = config.alarmCount++;
  if (index < 0) { giveStateLock(); sendError(request, id ? 404 : 409, id ? "Alarm not found" : "Alarm limit reached"); return; }
  AlarmRecord &alarm = config.alarms[index];
  if (!id) { memset(&alarm, 0, sizeof(alarm)); alarm.id = config.nextId++; if (!config.nextId) config.nextId = 1; }
  memcpy(alarm.title, title, sizeof(alarm.title));
  alarm.hour = static_cast<uint8_t>(hour); alarm.minute = static_cast<uint8_t>(minute);
  alarm.weekdays = static_cast<uint8_t>(days); alarm.onceDate = static_cast<uint32_t>(once); alarm.enabled = static_cast<uint8_t>(enabled);
  lastFiredOccurrence[index] = 0;
  markConfigDirty();
  giveStateLock();
  request->send(200, "application/json", "{\"ok\":true}");
}

static void handleDeleteAlarm(AsyncWebServerRequest *request) {
  int64_t id = 0;
  if (!parseIntArgument(request, "id", 1, INT64_MAX, id)) { sendError(request, 400, "Invalid alarm id"); return; }
  if (!takeStateLock()) { sendError(request, 503, "Device busy; retry shortly"); return; }
  int index = -1;
  for (uint8_t i = 0; i < config.alarmCount; ++i) if (config.alarms[i].id == static_cast<uint64_t>(id)) { index = i; break; }
  if (index < 0) { giveStateLock(); sendError(request, 404, "Alarm not found"); return; }
  uint16_t lowerMask = static_cast<uint16_t>((1U << index) - 1U);
  uint16_t upperMask = static_cast<uint16_t>(ringingAlarmMask >> (index + 1));
  ringingAlarmMask = static_cast<uint16_t>((ringingAlarmMask & lowerMask) | (upperMask << index));
  if (!ringingAlarmMask) { alarmRinging = false; snoozeUntilEpoch = 0; }
  for (uint8_t i = static_cast<uint8_t>(index); i + 1 < config.alarmCount; ++i) { config.alarms[i] = config.alarms[i + 1]; lastFiredOccurrence[i] = lastFiredOccurrence[i + 1]; } 
  memset(&config.alarms[--config.alarmCount], 0, sizeof(AlarmRecord));
  lastFiredOccurrence[config.alarmCount] = 0;
  markConfigDirty();
  giveStateLock();
  request->send(200, "application/json", "{\"ok\":true}");
}

static void handleSaveEvent(AsyncWebServerRequest *request) {
  int64_t id = 0, date = 0;
  bool idPresent = false;
  String idText = getArgument(request, "id", idPresent);
  if (idPresent) { char *end = nullptr; long long parsed = strtoll(idText.c_str(), &end, 10); if (!idText.length() || *end || parsed < 0) { sendError(request, 400, "Invalid event id"); return; } id = parsed; }
  if (!parseIntArgument(request, "date", 20000101, 20991231, date) || !validDate(static_cast<uint32_t>(date))) { sendError(request, 400, "Invalid calendar date"); return; }
  char title[49] = {};
  if (!copyTextArgument(request, "title", title, sizeof(title))) { sendError(request, 400, "Event title must contain 1 to 48 characters"); return; }
  if (!takeStateLock()) { sendError(request, 503, "Device busy; retry shortly"); return; }
  int index = -1;
  if (id) for (uint8_t i = 0; i < config.eventCount; ++i) if (config.events[i].id == static_cast<uint64_t>(id)) { index = i; break; }
  if (!id && config.eventCount < MAX_EVENTS) index = config.eventCount++;
  if (index < 0) { giveStateLock(); sendError(request, id ? 404 : 409, id ? "Event not found" : "Calendar event limit reached"); return; }
  EventRecord &event = config.events[index];
  if (!id) { memset(&event, 0, sizeof(event)); event.id = config.nextId++; if (!config.nextId) config.nextId = 1; }
  event.date = static_cast<uint32_t>(date);
  memcpy(event.title, title, sizeof(event.title));
  markConfigDirty();
  giveStateLock();
  request->send(200, "application/json", "{\"ok\":true}");
}

static void handleDeleteEvent(AsyncWebServerRequest *request) {
  int64_t id = 0;
  if (!parseIntArgument(request, "id", 1, INT64_MAX, id)) { sendError(request, 400, "Invalid event id"); return; }
  if (!takeStateLock()) { sendError(request, 503, "Device busy; retry shortly"); return; }
  int index = -1;
  for (uint8_t i = 0; i < config.eventCount; ++i) if (config.events[i].id == static_cast<uint64_t>(id)) { index = i; break; }
  if (index < 0) { giveStateLock(); sendError(request, 404, "Event not found"); return; }
  for (uint8_t i = static_cast<uint8_t>(index); i + 1 < config.eventCount; ++i) config.events[i] = config.events[i + 1];
  memset(&config.events[--config.eventCount], 0, sizeof(EventRecord));
  markConfigDirty();
  giveStateLock();
  request->send(200, "application/json", "{\"ok\":true}");
}

static void handleAlarmAction(AsyncWebServerRequest *request) {
  bool found = false;
  String action = getArgument(request, "action", found);
  if (!found || (action != "dismiss" && action != "snooze")) { sendError(request, 400, "Action must be dismiss or snooze"); return; }
  int64_t minutes = 5;
  if (action == "snooze" && !parseIntArgument(request, "minutes", 1, 30, minutes, false)) { sendError(request, 400, "Snooze must be between 1 and 30 minutes"); return; }
  if (!takeStateLock()) { sendError(request, 503, "Device busy; retry shortly"); return; }
  if (action == "snooze") { alarmRinging = false; snoozeUntilEpoch = currentEpoch() + minutes * 60LL; }
  else { alarmRinging = false; snoozeUntilEpoch = 0; ringingAlarmMask = 0; }
  giveStateLock();
  request->send(200, "application/json", "{\"ok\":true}");
}

static void handleFactoryReset(AsyncWebServerRequest *request) {
  if (!takeStateLock()) { sendError(request, 503, "Device busy; retry shortly"); return; }
  setDefaults(config);
  memset(lastFiredOccurrence, 0, sizeof(lastFiredOccurrence));
  alarmRinging = false; snoozeUntilEpoch = 0; ringingAlarmMask = 0;
  markConfigDirty(true);
  restartWhenSaved = true; restartAtMs = millis() + 2000UL;
  giveStateLock();
  request->send(202, "application/json", "{\"ok\":true,\"restarting\":true}");
}

static void setupRoutes() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html; charset=utf-8", reinterpret_cast<const uint8_t *>(INDEX_HTML), sizeof(INDEX_HTML) - 1U);
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("X-Content-Type-Options", "nosniff");
    request->send(response);
  });
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/time", HTTP_POST, handleSetTime);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.on("/api/alarms", HTTP_POST, handleSaveAlarm);
  server.on("/api/alarms", HTTP_DELETE, handleDeleteAlarm);
  server.on("/api/events", HTTP_POST, handleSaveEvent);
  server.on("/api/events", HTTP_DELETE, handleDeleteEvent);
  server.on("/api/alarm/action", HTTP_POST, handleAlarmAction);
  server.on("/api/factory-reset", HTTP_POST, handleFactoryReset);
  const char *captivePaths[] = {"/generate_204", "/gen_204", "/hotspot-detect.html", "/canonical.html", "/success.txt", "/connecttest.txt", "/ncsi.txt", "/fwlink"};
  for (const char *path : captivePaths) server.on(path, HTTP_GET, [](AsyncWebServerRequest *request) { request->redirect("http://192.168.4.1/"); });
  server.onNotFound([](AsyncWebServerRequest *request) { request->redirect("http://192.168.4.1/"); });
  server.begin();
}

static void persistConfigurationIfDue() {
  uint32_t now = millis();
  if (!takeStateLock()) return;
  bool due = configDirty && static_cast<uint32_t>(now - dirtyAtMs) >= CONFIG_DEBOUNCE_MS &&
    (configForceWrite || !configEverWrittenThisBoot || static_cast<uint32_t>(now - lastWriteAtMs) >= CONFIG_MIN_WRITE_MS);
  if (!due) { giveStateLock(); return; }
  DeviceConfig snapshot = config;
  uint64_t generation = changeGeneration;
  giveStateLock();
  snapshot.sequence = storedSequence + 1ULL;
  snapshot.magic = CONFIG_MAGIC;
  snapshot.version = CONFIG_VERSION;
  snapshot.bytes = sizeof(DeviceConfig);
  snapshot.crc = crc32Bytes(reinterpret_cast<const uint8_t *>(&snapshot), offsetof(DeviceConfig, crc));
  const char *slot = (snapshot.sequence & 1ULL) ? "slot1" : "slot0";
  size_t written = preferences.putBytes(slot, &snapshot, sizeof(snapshot));
  DeviceConfig verification{};
  bool okay = written == sizeof(snapshot) && preferences.getBytes(slot, &verification, sizeof(verification)) == sizeof(verification) && configValid(verification);
  if (okay) {
    storedSequence = snapshot.sequence;
    lastWriteAtMs = now;
    configEverWrittenThisBoot = true;
    if (takeStateLock()) {
      config.sequence = snapshot.sequence;
      if (changeGeneration == generation) { configDirty = false; configForceWrite = false; }
      giveStateLock();
    }
  } else if (takeStateLock()) {
    configDirty = true;
    configForceWrite = true;
    dirtyAtMs = millis();
    giveStateLock();
  }
}

static void tryRtcWrite() {
  bool pending = false, available = false;
  if (!takeStateLock()) return;
  pending = rtcSyncPending;
  available = rtcAvailable;
  giveStateLock();
  if (!pending || !available) return;
  int64_t epoch = currentEpoch();
  if (!timeIsUsable(epoch)) return;
  rtc.adjust(DateTime(static_cast<uint32_t>(epoch - 946684800LL)));
  if (takeStateLock()) {
    rtcSyncPending = false;
    lastRtcRefreshEpoch = epoch;
    giveStateLock();
  }
}

static void checkRtcProbe() {
  uint32_t nowMs = millis();
  if (static_cast<int32_t>(nowMs - nextRtcProbeMs) < 0) return;
  nextRtcProbeMs = nowMs + RTC_PROBE_INTERVAL_MS;
  bool available = false;
  if (!takeStateLock()) return;
  available = rtcAvailable;
  giveStateLock();
  if (available) {
    DateTime check = rtc.now();
    if (!timeIsUsable(static_cast<int64_t>(check.unixtime())) && takeStateLock()) { rtcAvailable = false; giveStateLock(); }
    return;
  }
  bool detected = rtc.begin();
  if (!detected) return;
  DateTime rtcNow = rtc.now();
  int64_t epoch = static_cast<int64_t>(rtcNow.unixtime());
  bool rtcValid = !rtc.lostPower() && timeIsUsable(epoch);
  if (!takeStateLock()) return;
  rtcAvailable = true;
  if (rtcValid && !timeValid) { setSystemEpoch(epoch); timeValid = true; }
  else if (timeValid) rtcSyncPending = true;
  giveStateLock();
}

static uint32_t dateKeyFromTm(const struct tm &local) {
  return static_cast<uint32_t>((local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday);
}

static void runAlarmScheduler() {
  uint32_t nowMs = millis();
  if (static_cast<uint32_t>(nowMs - lastSchedulerMs) < 200UL) return;
  lastSchedulerMs = nowMs;
  int64_t epoch = currentEpoch();
  bool output = false;
  if (!takeStateLock()) return;
  if (!timeValid || !timeIsUsable(epoch)) {
    alarmRinging = false;
    snoozeUntilEpoch = 0;
    ringingAlarmMask = 0;
  } else {
    if (snoozeUntilEpoch > 0 && epoch >= snoozeUntilEpoch) { snoozeUntilEpoch = 0; alarmRinging = true; }
    int64_t localEpoch = epoch + static_cast<int64_t>(config.timezoneMinutes) * 60LL;
    time_t localTime = static_cast<time_t>(localEpoch);
    struct tm localParts{};
    gmtime_r(&localTime, &localParts);
    uint32_t date = dateKeyFromTm(localParts);
    uint64_t occurrence = static_cast<uint64_t>(date) * 1440ULL + static_cast<uint64_t>(localParts.tm_hour * 60 + localParts.tm_min);
    for (uint8_t i = 0; i < config.alarmCount; ++i) {
      AlarmRecord &a = config.alarms[i];
      if (!a.enabled) continue;
      if (a.onceDate && a.onceDate < date) { a.enabled = 0; markConfigDirty(); continue; }
      if (a.hour != localParts.tm_hour || a.minute != localParts.tm_min || lastFiredOccurrence[i] == occurrence) continue;
      bool matches = a.onceDate ? a.onceDate == date : ((a.weekdays & (1U << localParts.tm_wday)) != 0);
      if (!matches) continue;
      lastFiredOccurrence[i] = occurrence;
      ringingAlarmMask |= static_cast<uint16_t>(1U << i);
      alarmRinging = true;
      snoozeUntilEpoch = 0;
      if (a.onceDate) { a.enabled = 0; markConfigDirty(); }
    }
  }
  output = alarmRinging;
  giveStateLock();
  if (output != alarmOutputState) { digitalWrite(ALARM_OUTPUT_PIN, output ? HIGH : LOW); alarmOutputState = output; }
}

static void handleResetButton() {
  bool pressed = digitalRead(FACTORY_RESET_PIN) == LOW;
  uint32_t now = millis();
  if (!pressed) { resetButtonTiming = false; resetLatchedUntilRelease = false; return; }
  if (resetLatchedUntilRelease) return;
  if (!resetButtonTiming) { resetButtonTiming = true; resetPressAtMs = now; return; }
  if (static_cast<uint32_t>(now - resetPressAtMs) >= 5000UL) {
    if (!takeStateLock()) return;
    resetLatchedUntilRelease = true;
    setDefaults(config);
    memset(lastFiredOccurrence, 0, sizeof(lastFiredOccurrence));
    alarmRinging = false; snoozeUntilEpoch = 0; ringingAlarmMask = 0;
    markConfigDirty(true);
    restartWhenSaved = true; restartAtMs = now + 2000UL;
    giveStateLock();
  }
}

static void checkPendingRestart() {
  bool restart = false;
  if (!takeStateLock()) return;
  restart = restartWhenSaved && !configDirty && static_cast<int32_t>(millis() - restartAtMs) >= 0;
  giveStateLock();
  if (restart) ESP.restart();
}

void setup() {
  stateMutex = xSemaphoreCreateMutex();
  pinMode(ALARM_OUTPUT_PIN, OUTPUT);
  digitalWrite(ALARM_OUTPUT_PIN, LOW);
  pinMode(FACTORY_RESET_PIN, INPUT_PULLUP);
  preferences.begin("alarmclock", false);
  loadConfiguration();
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(50);
  rtcAvailable = rtc.begin();
  if (rtcAvailable) {
    DateTime rtcNow = rtc.now();
    int64_t epoch = static_cast<int64_t>(rtcNow.unixtime());
    if (!rtc.lostPower() && timeIsUsable(epoch)) { setSystemEpoch(epoch); timeValid = true; lastRtcRefreshEpoch = epoch; }
  }
  if (!timeValid) {
    int64_t retainedEpoch = currentEpoch();
    if (timeIsUsable(retainedEpoch)) { timeValid = true; if (rtcAvailable) rtcSyncPending = true; }
  }
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  if (!WiFi.softAP(config.ssid, config.password, 6, false, 8)) {
    WiFi.softAP("ESP32_ALARM_CLOCK", "12345678", 6, false, 8);
  }
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", IPAddress(192,168,4,1));
  setupRoutes();
  nextRtcProbeMs = millis() + RTC_PROBE_INTERVAL_MS;
}

void loop() {
  dnsServer.processNextRequest();
  handleResetButton();
  checkRtcProbe();
  uint32_t nowMs = millis();
  if (static_cast<uint32_t>(nowMs - lastRtcRefreshCheckMs) >= 1000UL) {
    lastRtcRefreshCheckMs = nowMs;
    int64_t epoch = currentEpoch();
    if (takeStateLock()) {
      if (rtcAvailable && timeIsUsable(epoch) && lastRtcRefreshEpoch > 0 && epoch - lastRtcRefreshEpoch >= RTC_REFRESH_INTERVAL_SECONDS) rtcSyncPending = true;
      giveStateLock();
    }
  }
  tryRtcWrite();
  runAlarmScheduler();
  persistConfigurationIfDue();
  checkPendingRestart();
  yield();
}
