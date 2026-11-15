#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <errno.h>

#define SDA_PIN 21
#define SCL_PIN 22
#define ALARM_PIN 25
#define FACTORY_RESET_PIN 0
#define DS3231_ADDR 0x68
#define MAX_ALARMS 8
#define ALARM_NAME_LEN 20
#define CONFIG_MAGIC 0x414C524DUL
#define CONFIG_VERSION 2
#define NVS_NAMESPACE "alarmclock"
#define NVS_KEY_A "cfgA"
#define NVS_KEY_B "cfgB"
#define CONFIG_SAVE_DEBOUNCE_MS 30000ULL
#define RTC_READ_INTERVAL_MS 1000ULL
#define RTC_AUX_INTERVAL_MS 10000ULL
#define FACTORY_HOLD_MS 5000ULL

static const char AP_SSID_DEFAULT[] = "ESP32_Alarm_Clock";
static const char AP_PASS_DEFAULT[] = "12345678";

struct TimeParts {
  int16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  uint8_t weekday;
};

struct Alarm {
  uint8_t enabled;
  uint8_t hour;
  uint8_t minute;
  uint8_t daysMask;
  uint16_t durationSec;
  uint8_t reserved[2];
  char name[ALARM_NAME_LEN];
};

struct Config {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t seq;
  char apSsid[33];
  char apPass[64];
  uint8_t apChannel;
  uint8_t apHidden;
  uint8_t apMaxConn;
  uint8_t outputActiveHigh;
  int16_t tzOffsetMinutes;
  uint8_t use24Hour;
  uint8_t alarmCount;
  Alarm alarms[MAX_ALARMS];
  uint32_t crc;
};

extern const char APP_HTML[] PROGMEM;
extern const char STYLE_CSS[] PROGMEM;
extern const char APP_JS[] PROGMEM;

static Config cfg;
static WebServer server(80);
static DNSServer dnsServer;

static bool configDirty = false;
static bool configForceSave = false;
static uint64_t configDirtySinceMs = 0;

static bool rtcValid = false;
static uint64_t rtcUtcEpoch = 0;
static uint64_t rtcLocalEpoch = 0;
static TimeParts rtcUtcParts;
static TimeParts rtcLocalParts;
static float rtcTempC = NAN;
static uint8_t rtcStatusReg = 0;
static uint8_t rtcControlReg = 0;
static uint8_t rtcAgingReg = 0;
static bool rtcOsf = false;
static uint64_t lastRtcReadMs = 0;
static uint64_t lastRtcAuxReadMs = 0;

static bool alarmOutputActive = false;
static uint64_t alarmOutputOffAtMs = 0;
static int16_t alarmActiveIndex = -1;
static uint64_t lastTriggerMinute[MAX_ALARMS];
static bool alarmPrimed = false;

static bool apRestartPending = false;
static uint64_t apRestartAtMs = 0;
static bool factoryResetScheduled = false;
static uint64_t factoryResetScheduledAtMs = 0;

static const char* WEEKDAY_SHORT[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static uint64_t nowMs64() {
  return (uint64_t)(esp_timer_get_time() / 1000);
}

static bool isLeapYear(int y) {
  return ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
}

static uint8_t daysInMonth(int y, uint8_t m) {
  static const uint8_t md[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m < 1 || m > 12) return 0;
  if (m == 2 && isLeapYear(y)) return 29;
  return md[m - 1];
}

static void epochToParts(uint64_t epoch, TimeParts& tp) {
  int64_t days = (int64_t)(epoch / 86400ULL);
  uint32_t sod = (uint32_t)(epoch % 86400ULL);
  tp.hour = (uint8_t)(sod / 3600U);
  tp.minute = (uint8_t)((sod % 3600U) / 60U);
  tp.second = (uint8_t)(sod % 60U);

  int64_t z = days + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  uint32_t doe = (uint32_t)(z - era * 146097);
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int32_t y = (int32_t)yoe + (int32_t)(era * 400);
  uint32_t doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
  uint32_t mp = (5U * doy + 2U) / 153U;
  uint32_t d = doy - (153U * mp + 2U) / 5U + 1U;
  uint32_t m = (mp < 10U ? mp + 3U : mp - 9U);
  y += (m <= 2U);

  tp.year = (int16_t)y;
  tp.month = (uint8_t)m;
  tp.day = (uint8_t)d;

  int wd = (int)((days + 4) % 7);
  if (wd < 0) wd += 7;
  tp.weekday = (uint8_t)wd;
}

static uint64_t partsToEpoch(int year, int month, int day, int hour, int minute, int second) {
  int y = year - (month <= 2 ? 1 : 0);
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned mp = (unsigned)(month + (month > 2 ? -3 : 9));
  unsigned doy = (153U * mp + 2U) / 5U + (unsigned)(day - 1);
  unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
  int64_t days = (int64_t)era * 146097LL + (int64_t)doe - 719468LL;
  return (uint64_t)(days * 86400LL + (int64_t)hour * 3600LL + (int64_t)minute * 60LL + (int64_t)second);
}

static bool isTextString(const char* s, size_t maxLen) {
  for (size_t i = 0; i < maxLen; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == 0) return true;
    if (c < 32 || c == 127) return false;
  }
  return false;
}

static bool validSSID(const char* s) {
  for (size_t i = 0; i < 33; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == 0) return i >= 1 && i <= 32;
    if (c < 32 || c > 126) return false;
    if (i >= 32) return false;
  }
  return false;
}

static bool validPass(const char* s) {
  for (size_t i = 0; i < 64; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == 0) {
      if (i == 0) return true;
      return i >= 8 && i <= 63;
    }
    if (c < 32 || c > 126) return false;
    if (i >= 63) return false;
  }
  return false;
}

static bool copyStringSafe(char* dst, size_t cap, const char* src) {
  if (cap == 0) return false;
  size_t i = 0;
  if (src) {
    while (i + 1 < cap && src[i]) {
      dst[i] = src[i];
      i++;
    }
  }
  dst[i] = 0;
  return true;
}

static void copySanitizedString(char* dst, size_t cap, const char* src) {
  if (cap == 0) return;
  size_t j = 0;
  if (src) {
    for (size_t i = 0; src[i] && j + 1 < cap; i++) {
      unsigned char c = (unsigned char)src[i];
      if (c >= 32 && c != 127) {
        dst[j++] = (char)c;
      }
    }
  }
  dst[j] = 0;
}

static uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      uint32_t mask = (crc & 1U) ? 0xFFFFFFFFUL : 0U;
      crc = (crc >> 1) ^ (0xEDB88320UL & mask);
    }
  }
  return ~crc;
}

static void setDefaults(Config& c) {
  memset(&c, 0, sizeof(c));
  c.magic = CONFIG_MAGIC;
  c.version = CONFIG_VERSION;
  c.size = (uint16_t)sizeof(Config);
  c.seq = 1;
  copyStringSafe(c.apSsid, sizeof(c.apSsid), AP_SSID_DEFAULT);
  copyStringSafe(c.apPass, sizeof(c.apPass), AP_PASS_DEFAULT);
  c.apChannel = 1;
  c.apHidden = 0;
  c.apMaxConn = 4;
  c.outputActiveHigh = 1;
  c.tzOffsetMinutes = 0;
  c.use24Hour = 1;
  c.alarmCount = 0;
}

static bool validateConfig(const Config& c) {
  if (c.magic != CONFIG_MAGIC) return false;
  if (c.version != CONFIG_VERSION) return false;
  if (c.size != (uint16_t)sizeof(Config)) return false;
  uint32_t expected = crc32((const uint8_t*)&c, offsetof(Config, crc));
  if (expected != c.crc) return false;
  if (!validSSID(c.apSsid)) return false;
  if (!validPass(c.apPass)) return false;
  if (c.apChannel < 1 || c.apChannel > 13) return false;
  if (c.apHidden > 1) return false;
  if (c.apMaxConn < 1 || c.apMaxConn > 8) return false;
  if (c.outputActiveHigh > 1) return false;
  if (c.use24Hour > 1) return false;
  if (c.tzOffsetMinutes < -840 || c.tzOffsetMinutes > 840) return false;
  if (c.alarmCount > MAX_ALARMS) return false;
  for (uint8_t i = 0; i < c.alarmCount; i++) {
    const Alarm& a = c.alarms[i];
    if (a.enabled > 1) return false;
    if (a.hour > 23 || a.minute > 59) return false;
    if (a.daysMask & 0x80) return false;
    if (a.durationSec == 0) return false;
    if (!isTextString(a.name, sizeof(a.name))) return false;
  }
  return true;
}

static bool readConfigSlot(Preferences& p, const char* key, Config& out) {
  size_t len = p.getBytesLength(key);
  if (len != sizeof(Config)) return false;
  if (p.getBytes(key, &out, sizeof(Config)) != sizeof(Config)) return false;
  return validateConfig(out);
}

static bool saveConfigToNVS() {
  Config cur;
  memset(&cur, 0, sizeof(cur));
  cur.magic = CONFIG_MAGIC;
  cur.version = CONFIG_VERSION;
  cur.size = (uint16_t)sizeof(Config);
  copyStringSafe(cur.apSsid, sizeof(cur.apSsid), cfg.apSsid);
  copyStringSafe(cur.apPass, sizeof(cur.apPass), cfg.apPass);
  cur.apChannel = cfg.apChannel;
  cur.apHidden = cfg.apHidden ? 1 : 0;
  cur.apMaxConn = cfg.apMaxConn;
  cur.outputActiveHigh = cfg.outputActiveHigh ? 1 : 0;
  cur.tzOffsetMinutes = cfg.tzOffsetMinutes;
  cur.use24Hour = cfg.use24Hour ? 1 : 0;
  cur.alarmCount = cfg.alarmCount;
  if (cur.alarmCount > MAX_ALARMS) cur.alarmCount = MAX_ALARMS;
  for (uint8_t i = 0; i < MAX_ALARMS; i++) {
    memset(&cur.alarms[i], 0, sizeof(Alarm));
    if (i < cur.alarmCount) {
      cur.alarms[i] = cfg.alarms[i];
      cur.alarms[i].enabled = cur.alarms[i].enabled ? 1 : 0;
      cur.alarms[i].name[sizeof(cur.alarms[i].name) - 1] = 0;
    }
  }

  Preferences p;
  if (!p.begin(NVS_NAMESPACE, false)) return false;

  Config a;
  Config b;
  bool va = readConfigSlot(p, NVS_KEY_A, a);
  bool vb = readConfigSlot(p, NVS_KEY_B, b);

  uint32_t nextSeq = 1;
  if (va && vb) {
    nextSeq = (a.seq > b.seq ? a.seq : b.seq) + 1;
  } else if (va) {
    nextSeq = a.seq + 1;
  } else if (vb) {
    nextSeq = b.seq + 1;
  }

  cur.seq = nextSeq;
  cur.crc = 0;
  cur.crc = crc32((const uint8_t*)&cur, offsetof(Config, crc));

  if (!validateConfig(cur)) {
    p.end();
    return false;
  }

  const char* target = NVS_KEY_A;
  if (va && vb) {
    target = (a.seq <= b.seq) ? NVS_KEY_A : NVS_KEY_B;
  } else if (va) {
    target = NVS_KEY_B;
  } else if (vb) {
    target = NVS_KEY_A;
  }

  size_t written = p.putBytes(target, &cur, sizeof(Config));
  p.end();

  if (written != sizeof(Config)) return false;
  cfg = cur;
  return true;
}

static void loadConfig() {
  bool va = false;
  bool vb = false;
  Config a;
  Config b;

  Preferences p;
  if (p.begin(NVS_NAMESPACE, true)) {
    va = readConfigSlot(p, NVS_KEY_A, a);
    vb = readConfigSlot(p, NVS_KEY_B, b);
    p.end();
  }

  bool have = false;
  if (va && vb) {
    if ((int32_t)(a.seq - b.seq) >= 0) cfg = a;
    else cfg = b;
    have = true;
  } else if (va) {
    cfg = a;
    have = true;
  } else if (vb) {
    cfg = b;
    have = true;
  }

  if (!have) {
    setDefaults(cfg);
    saveConfigToNVS();
  } else {
    cfg.apSsid[sizeof(cfg.apSsid) - 1] = 0;
    cfg.apPass[sizeof(cfg.apPass) - 1] = 0;
    for (uint8_t i = 0; i < MAX_ALARMS; i++) {
      cfg.alarms[i].name[sizeof(cfg.alarms[i].name) - 1] = 0;
    }
  }
}

static void requestConfigSave(bool force = false) {
  configDirty = true;
  configDirtySinceMs = nowMs64();
  if (force) configForceSave = true;
}

static void processConfigSave(uint64_t now) {
  if (!configDirty) return;
  if (configForceSave || (now - configDirtySinceMs >= CONFIG_SAVE_DEBOUNCE_MS)) {
    if (saveConfigToNVS()) {
      configDirty = false;
      configForceSave = false;
    } else {
      configDirtySinceMs = now;
      configForceSave = false;
    }
  }
}

static void primeAlarmTriggers(uint64_t localEpoch) {
  uint64_t minute = localEpoch / 60ULL;
  for (uint8_t i = 0; i < MAX_ALARMS; i++) {
    lastTriggerMinute[i] = minute;
  }
}

static void reprimeAlarms() {
  if (rtcValid) {
    primeAlarmTriggers(rtcLocalEpoch);
    alarmPrimed = true;
  } else {
    alarmPrimed = false;
  }
}

static void triggerAlarmOutput(uint16_t durationSec, int16_t index, uint64_t now) {
  uint64_t off = now + (uint64_t)(durationSec ? durationSec : 1U) * 1000ULL;
  if (!alarmOutputActive || off > alarmOutputOffAtMs) {
    alarmOutputOffAtMs = off;
  }
  alarmOutputActive = true;
  alarmActiveIndex = index;
  digitalWrite(ALARM_PIN, cfg.outputActiveHigh ? HIGH : LOW);
}

static void stopAlarmOutput() {
  alarmOutputActive = false;
  alarmActiveIndex = -1;
  digitalWrite(ALARM_PIN, cfg.outputActiveHigh ? LOW : HIGH);
}

static void processAlarms(const TimeParts& local, uint64_t localEpoch, uint64_t now) {
  if (!alarmPrimed) {
    primeAlarmTriggers(localEpoch);
    alarmPrimed = true;
    return;
  }

  uint64_t minute = localEpoch / 60ULL;
  uint8_t count = cfg.alarmCount;
  if (count > MAX_ALARMS) count = MAX_ALARMS;

  for (uint8_t i = 0; i < count; i++) {
    const Alarm& a = cfg.alarms[i];
    if (!a.enabled) continue;
    if (!(a.daysMask & (1U << local.weekday))) continue;
    if (a.hour != local.hour || a.minute != local.minute) continue;
    if (lastTriggerMinute[i] == minute) continue;
    lastTriggerMinute[i] = minute;
    triggerAlarmOutput(a.durationSec, (int16_t)i, now);
  }

  if (alarmOutputActive && now >= alarmOutputOffAtMs) {
    stopAlarmOutput();
  }
}

static uint8_t bcd2dec(uint8_t v) {
  return (uint8_t)(((v >> 4) * 10U) + (v & 0x0FU));
}

static uint8_t dec2bcd(uint8_t v) {
  return (uint8_t)(((v / 10U) << 4) | (v % 10U));
}

static bool i2cReadBytes(uint8_t reg, uint8_t* data, uint8_t len) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  int got = Wire.requestFrom((int)DS3231_ADDR, (int)len);
  if (got != len) return false;
  for (uint8_t i = 0; i < len; i++) {
    data[i] = Wire.read();
  }
  return true;
}

static bool i2cReadByte(uint8_t reg, uint8_t* value) {
  return i2cReadBytes(reg, value, 1);
}

static bool i2cWriteByte(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission(true) == 0;
}

static bool i2cWriteBytes(uint8_t reg, const uint8_t* data, uint8_t len) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(reg);
  for (uint8_t i = 0; i < len; i++) {
    Wire.write(data[i]);
  }
  return Wire.endTransmission(true) == 0;
}

static bool decodeDS3231(const uint8_t* raw, TimeParts& tp, uint64_t& utc) {
  uint8_t secReg = raw[0] & 0x7F;
  uint8_t minReg = raw[1] & 0x7F;
  if ((secReg & 0x0F) > 9 || (secReg >> 4) > 5) return false;
  if ((minReg & 0x0F) > 9 || (minReg >> 4) > 5) return false;

  uint8_t sec = bcd2dec(secReg);
  uint8_t minute = bcd2dec(minReg);

  uint8_t hourReg = raw[2];
  uint8_t hourBcd;
  uint8_t hour;

  if (hourReg & 0x40) {
    hourBcd = hourReg & 0x1F;
    if ((hourBcd & 0x0F) > 9 || (hourBcd >> 4) > 1) return false;
    hour = bcd2dec(hourBcd);
    if (hour == 0 || hour > 12) return false;
    if (hour == 12) hour = 0;
    if (hourReg & 0x20) hour += 12;
  } else {
    hourBcd = hourReg & 0x3F;
    if ((hourBcd & 0x0F) > 9 || (hourBcd >> 4) > 2) return false;
    hour = bcd2dec(hourBcd);
    if (hour > 23) return false;
    if ((hourBcd >> 4) == 2 && (hourBcd & 0x0F) > 3) return false;
  }

  uint8_t dayReg = raw[4] & 0x3F;
  if ((dayReg & 0x0F) > 9 || (dayReg >> 4) > 3) return false;
  uint8_t day = bcd2dec(dayReg);
  if (day == 0 || day > 31) return false;

  uint8_t monthReg = raw[5] & 0x1F;
  if ((monthReg & 0x0F) > 9 || (monthReg >> 4) > 1) return false;
  uint8_t month = bcd2dec(monthReg);
  if (month < 1 || month > 12) return false;
  bool century = (raw[5] & 0x80) != 0;

  uint8_t yearReg = raw[6];
  if ((yearReg & 0x0F) > 9 || (yearReg >> 4) > 9) return false;
  uint8_t year2 = bcd2dec(yearReg);
  int year = 2000 + (int)year2 + (century ? 100 : 0);

  if (sec > 59 || minute > 59 || hour > 23) return false;
  if (day > daysInMonth(year, month)) return false;

  utc = partsToEpoch(year, month, day, hour, minute, sec);
  TimeParts tmp;
  epochToParts(utc, tmp);

  tp.year = (int16_t)year;
  tp.month = month;
  tp.day = day;
  tp.hour = hour;
  tp.minute = minute;
  tp.second = sec;
  tp.weekday = tmp.weekday;

  return true;
}

static bool setDS3231Epoch(uint64_t utc) {
  TimeParts tp;
  epochToParts(utc, tp);

  if (tp.year < 2000 || tp.year > 2199) return false;

  uint8_t raw[7];
  raw[0] = dec2bcd(tp.second);
  raw[1] = dec2bcd(tp.minute);
  raw[2] = dec2bcd(tp.hour);
  raw[3] = (uint8_t)(tp.weekday + 1U);
  raw[4] = dec2bcd(tp.day);
  uint8_t monthBcd = dec2bcd(tp.month);
  if (tp.year >= 2100) monthBcd |= 0x80;
  raw[5] = monthBcd;
  raw[6] = dec2bcd((uint8_t)(tp.year % 100));

  if (!i2cWriteBytes(0x00, raw, 7)) return false;

  uint8_t status;
  if (i2cReadByte(0x0F, &status)) {
    status &= (uint8_t)~0x80;
    i2cWriteByte(0x0F, status);
  }

  rtcValid = false;
  alarmPrimed = false;
  return true;
}

static void updateRTCAux() {
  lastRtcAuxReadMs = nowMs64();

  uint8_t status;
  if (i2cReadByte(0x0F, &status)) {
    rtcStatusReg = status;
    rtcOsf = (status & 0x80) != 0;
  }

  uint8_t control;
  if (i2cReadByte(0x0E, &control)) {
    rtcControlReg = control;
  }

  uint8_t aging;
  if (i2cReadByte(0x10, &aging)) {
    rtcAgingReg = aging;
  }

  uint8_t temp[2];
  if (i2cReadBytes(0x11, temp, 2)) {
    rtcTempC = (float)((int8_t)temp[0]) + (float)((temp[1] >> 6) & 0x03) * 0.25f;
  } else {
    rtcTempC = NAN;
  }
}

static void updateRTC() {
  uint64_t now = nowMs64();
  lastRtcReadMs = now;

  uint8_t raw[7];
  if (i2cReadBytes(0x00, raw, 7)) {
    TimeParts utcParts;
    uint64_t utc = 0;
    if (decodeDS3231(raw, utcParts, utc)) {
      rtcUtcEpoch = utc;
      rtcUtcParts = utcParts;

      int64_t localSigned = (int64_t)utc + (int64_t)cfg.tzOffsetMinutes * 60LL;
      if (localSigned < 0) localSigned = 0;

      rtcLocalEpoch = (uint64_t)localSigned;
      epochToParts(rtcLocalEpoch, rtcLocalParts);
      rtcValid = true;

      if (!alarmPrimed) {
        primeAlarmTriggers(rtcLocalEpoch);
        alarmPrimed = true;
      }

      processAlarms(rtcLocalParts, rtcLocalEpoch, now);
    } else {
      rtcValid = false;
    }
  } else {
    rtcValid = false;
  }

  if (alarmOutputActive && now >= alarmOutputOffAtMs) {
    stopAlarmOutput();
  }
}

static bool softAPStart(const char* ssid, const char* pass, uint8_t channel, bool hidden, uint8_t maxConn) {
  WiFi.mode(WIFI_AP);
  WiFi.softAPdisconnect(true);
  const char* p = (pass && pass[0]) ? pass : nullptr;
  return WiFi.softAP(ssid, p, channel, hidden ? 1 : 0, maxConn);
}

static void startAPFromConfig() {
  bool ok = softAPStart(cfg.apSsid, cfg.apPass, cfg.apChannel, cfg.apHidden != 0, cfg.apMaxConn);
  if (!ok) {
    copyStringSafe(cfg.apSsid, sizeof(cfg.apSsid), AP_SSID_DEFAULT);
    copyStringSafe(cfg.apPass, sizeof(cfg.apPass), AP_PASS_DEFAULT);
    cfg.apChannel = 1;
    cfg.apHidden = 0;
    cfg.apMaxConn = 4;
    requestConfigSave(true);
    ok = softAPStart(cfg.apSsid, cfg.apPass, cfg.apChannel, false, cfg.apMaxConn);
    if (!ok) {
      softAPStart(AP_SSID_DEFAULT, nullptr, 1, 0, 4);
    }
  }
  dnsServer.stop();
  dnsServer.start(53, "*", WiFi.softAPIP());
}

static void clearPersistentConfig() {
  Preferences p;
  if (p.begin(NVS_NAMESPACE, false)) {
    p.clear();
    p.end();
  }
}

static void restartDevice() {
  stopAlarmOutput();
  configDirty = false;
  configForceSave = false;
  WiFi.disconnect(true, true);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  ESP.restart();
}

static void performFactoryReset() {
  clearPersistentConfig();
  restartDevice();
}

static void scheduleAPApply() {
  apRestartPending = true;
  apRestartAtMs = nowMs64() + 2000ULL;
}

static void pollFactoryButton(uint64_t now) {
  static bool down = false;
  static uint64_t downAt = 0;
  static bool cleared = false;

  if (digitalRead(FACTORY_RESET_PIN) == LOW) {
    if (!down) {
      down = true;
      downAt = now;
      cleared = false;
    } else if (!cleared && (now - downAt >= FACTORY_HOLD_MS)) {
      cleared = true;
      clearPersistentConfig();
      configDirty = false;
      configForceSave = false;
      stopAlarmOutput();
    }
  } else {
    down = false;
    if (cleared) {
      cleared = false;
      restartDevice();
    }
  }
}

static void processScheduledTasks(uint64_t now) {
  if (factoryResetScheduled && now >= factoryResetScheduledAtMs) {
    factoryResetScheduled = false;
    performFactoryReset();
  }

  if (apRestartPending && now >= apRestartAtMs) {
    apRestartPending = false;
    startAPFromConfig();
  }
}

static bool parseLongArg(const char* name, long minV, long maxV, long* out, bool required) {
  if (!server.hasArg(name)) return !required;
  String v = server.arg(name);
  if (v.length() == 0 || v.length() != strlen(v.c_str())) return false;
  const char* s = v.c_str();
  char c0 = s[0];
  if (!((c0 >= '0' && c0 <= '9') || c0 == '-' || c0 == '+')) return false;
  char* end = nullptr;
  errno = 0;
  long x = strtol(s, &end, 10);
  if (end == s || *end != 0 || errno == ERANGE) return false;
  if (x < minV || x > maxV) return false;
  *out = x;
  return true;
}

static bool parseOptIntArg(const char* name, int minV, int maxV, bool* present, int* out) {
  if (!server.hasArg(name)) {
    *present = false;
    return true;
  }
  *present = true;
  long x;
  if (!parseLongArg(name, minV, maxV, &x, true)) return false;
  *out = (int)x;
  return true;
}

static bool parseULLArg(const char* name, unsigned long long minV, unsigned long long maxV, unsigned long long* out) {
  if (!server.hasArg(name)) return false;
  String v = server.arg(name);
  if (v.length() == 0 || v.length() != strlen(v.c_str())) return false;
  const char* s = v.c_str();
  if (s[0] < '0' || s[0] > '9') return false;
  char* end = nullptr;
  errno = 0;
  unsigned long long x = strtoull(s, &end, 10);
  if (end == s || *end != 0 || errno == ERANGE) return false;
  if (x < minV || x > maxV) return false;
  *out = x;
  return true;
}

struct JsonOut {
  char* p;
  size_t cap;
  size_t len;
  bool ok;
};

static void joInit(JsonOut* o, char* buf, size_t cap) {
  o->p = buf;
  o->cap = cap;
  o->len = 0;
  o->ok = (cap > 0);
  if (cap > 0) buf[0] = 0;
}

static void joChar(JsonOut* o, char c) {
  if (!o->ok) return;
  if (o->len + 1 >= o->cap) {
    o->ok = false;
    o->p[o->cap - 1] = 0;
    return;
  }
  o->p[o->len++] = c;
  o->p[o->len] = 0;
}

static void joRaw(JsonOut* o, const char* s) {
  if (!o->ok) return;
  size_t n = strlen(s);
  if (o->len + n >= o->cap) {
    o->ok = false;
    o->p[o->cap - 1] = 0;
    return;
  }
  memcpy(o->p + o->len, s, n);
  o->len += n;
  o->p[o->len] = 0;
}

static void joFmt(JsonOut* o, const char* fmt, ...) {
  if (!o->ok) return;

  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(o->p + o->len, o->cap - o->len, fmt, ap);
  va_end(ap);

  if (n < 0 || (size_t)n >= (o->cap - o->len)) {
    o->ok = false;
    o->p[o->cap - 1] = 0;
    return;
  }

  o->len += (size_t)n;
}

static void joStr(JsonOut* o, const char* s) {
  joChar(o, '"');
  if (s) {
    for (const char* p = s; *p; p++) {
      unsigned char c = (unsigned char)*p;
      if (c == '"' || c == '\\') {
        joChar(o, '\\');
        joChar(o, (char)c);
      } else if (c == '\n') {
        joRaw(o, "\\n");
      } else if (c == '\r') {
        joRaw(o, "\\r");
      } else if (c == '\t') {
        joRaw(o, "\\t");
      } else if (c < 32) {
        joFmt(o, "\\u%04x", c);
      } else {
        joChar(o, (char)c);
      }
    }
  }
  joChar(o, '"');
}

static void noCacheHeaders() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
}

static void sendBody(int code, const char* contentType, const char* body) {
  size_t len = strlen(body);
  server.setContentLength(len);
  server.send(code, contentType, "");
  if (len > 0) {
    server.sendContent(body);
  }
}

static void sendJsonOk() {
  noCacheHeaders();
  sendBody(200, "application/json", "{\"ok\":true}");
}

static void sendJsonError(const char* msg) {
  char b[160];
  snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"%s\"}", msg ? msg : "error");
  noCacheHeaders();
  sendBody(400, "application/json", b);
}

static void handleIndex() {
  noCacheHeaders();
  sendBody(200, "text/html", APP_HTML);
}

static void handleCss() {
  noCacheHeaders();
  sendBody(200, "text/css", STYLE_CSS);
}

static void handleJs() {
  noCacheHeaders();
  sendBody(200, "application/javascript", APP_JS);
}

static void handleStatus() {
  static char json[4096];
  JsonOut o;
  joInit(&o, json, sizeof(json));

  uint64_t now = nowMs64();

  char dateStr[40];
  char clockStr[24];
  char timeStr[80];
  char utcStr[80];

  if (rtcValid) {
    snprintf(dateStr, sizeof(dateStr), "%s %02u/%02u/%04d",
             WEEKDAY_SHORT[rtcLocalParts.weekday],
             (unsigned)rtcLocalParts.month,
             (unsigned)rtcLocalParts.day,
             (int)rtcLocalParts.year);

    snprintf(clockStr, sizeof(clockStr), "%02u:%02u:%02u",
             (unsigned)rtcLocalParts.hour,
             (unsigned)rtcLocalParts.minute,
             (unsigned)rtcLocalParts.second);

    snprintf(timeStr, sizeof(timeStr), "%s %s", dateStr, clockStr);

    snprintf(utcStr, sizeof(utcStr), "%s %02u/%02u/%04d %02u:%02u:%02u",
             WEEKDAY_SHORT[rtcUtcParts.weekday],
             (unsigned)rtcUtcParts.month,
             (unsigned)rtcUtcParts.day,
             (int)rtcUtcParts.year,
             (unsigned)rtcUtcParts.hour,
             (unsigned)rtcUtcParts.minute,
             (unsigned)rtcUtcParts.second);
  } else {
    strcpy(dateStr, "-- --- -- ----");
    strcpy(clockStr, "--:--:--");
    strcpy(timeStr, "-- --- -- ---- --:--:--");
    strcpy(utcStr, "--");
  }

  joRaw(&o, "{\"ok\":true");
  joFmt(&o, ",\"uptime\":%llu", (unsigned long long)(now / 1000ULL));
  joFmt(&o, ",\"heap\":%u", (unsigned int)ESP.getFreeHeap());
  joFmt(&o, ",\"rtcValid\":%s", rtcValid ? "true" : "false");
  joFmt(&o, ",\"utcEpoch\":%llu", (unsigned long long)rtcUtcEpoch);
  joFmt(&o, ",\"localEpoch\":%llu", (unsigned long long)rtcLocalEpoch);
  joFmt(&o, ",\"tzOffsetMinutes\":%d", (int)cfg.tzOffsetMinutes);
  joFmt(&o, ",\"year\":%d,\"month\":%u,\"day\":%u,\"hour\":%u,\"minute\":%u,\"second\":%u,\"weekday\":%u",
        (int)rtcLocalParts.year,
        (unsigned)rtcLocalParts.month,
        (unsigned)rtcLocalParts.day,
        (unsigned)rtcLocalParts.hour,
        (unsigned)rtcLocalParts.minute,
        (unsigned)rtcLocalParts.second,
        (unsigned)rtcLocalParts.weekday);
  joFmt(&o, ",\"utcYear\":%d,\"utcMonth\":%u,\"utcDay\":%u,\"utcHour\":%u,\"utcMinute\":%u,\"utcSecond\":%u",
        (int)rtcUtcParts.year,
        (unsigned)rtcUtcParts.month,
        (unsigned)rtcUtcParts.day,
        (unsigned)rtcUtcParts.hour,
        (unsigned)rtcUtcParts.minute,
        (unsigned)rtcUtcParts.second);

  joRaw(&o, ",\"dateStr\":");
  joStr(&o, dateStr);
  joRaw(&o, ",\"clockStr\":");
  joStr(&o, clockStr);
  joRaw(&o, ",\"timeStr\":");
  joStr(&o, timeStr);
  joRaw(&o, ",\"utcStr\":");
  joStr(&o, utcStr);

  joRaw(&o, ",\"temperature\":");
  if (isnan(rtcTempC)) joRaw(&o, "null");
  else joFmt(&o, "%.2f", rtcTempC);

  joFmt(&o, ",\"osf\":%s", rtcOsf ? "true" : "false");
  joFmt(&o, ",\"en32k\":%s", (rtcStatusReg & 0x08) ? "true" : "false");
  joFmt(&o, ",\"statusReg\":%u", (unsigned)rtcStatusReg);
  joFmt(&o, ",\"controlReg\":%u", (unsigned)rtcControlReg);
  joFmt(&o, ",\"agingReg\":%d", (int)(int8_t)rtcAgingReg);
  joFmt(&o, ",\"alarmOutput\":%s", alarmOutputActive ? "true" : "false");
  joFmt(&o, ",\"activeAlarm\":%d", (int)alarmActiveIndex);
  joFmt(&o, ",\"outputActiveHigh\":%s", cfg.outputActiveHigh ? "true" : "false");

  char ipbuf[20];
  IPAddress ip = WiFi.softAPIP();
  snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u", (unsigned)ip[0], (unsigned)ip[1], (unsigned)ip[2], (unsigned)ip[3]);

  joRaw(&o, ",\"ap\":{\"ssid\":");
  joStr(&o, cfg.apSsid);
  joRaw(&o, ",\"pass\":");
  joStr(&o, cfg.apPass);
  joFmt(&o, ",\"channel\":%u,\"hidden\":%s,\"maxConn\":%u,\"clients\":%d,\"ip\":",
        (unsigned)cfg.apChannel,
        cfg.apHidden ? "true" : "false",
        (unsigned)cfg.apMaxConn,
        WiFi.softAPgetStationNum());
  joStr(&o, ipbuf);
  joRaw(&o, "}");

  joFmt(&o, ",\"alarmCount\":%u,\"alarms\":[", (unsigned)cfg.alarmCount);

  uint8_t count = cfg.alarmCount;
  if (count > MAX_ALARMS) count = MAX_ALARMS;

  for (uint8_t i = 0; i < count; i++) {
    if (i) joChar(&o, ',');
    const Alarm& a = cfg.alarms[i];
    joFmt(&o, "{\"index\":%u,\"enabled\":%s,\"hour\":%u,\"minute\":%u,\"daysMask\":%u,\"duration\":%u,\"name\":",
          (unsigned)i,
          a.enabled ? "true" : "false",
          (unsigned)a.hour,
          (unsigned)a.minute,
          (unsigned)a.daysMask,
          (unsigned)a.durationSec);
    joStr(&o, a.name);
    joChar(&o, '}');
  }

  joRaw(&o, "]");
  joRaw(&o, "}");

  if (!o.ok) {
    noCacheHeaders();
    sendBody(500, "application/json", "{\"ok\":false,\"error\":\"json overflow\"}");
    return;
  }

  noCacheHeaders();
  sendBody(200, "application/json", json);
}

static void handleSyncTime() {
  unsigned long long ev;
  if (!parseULLArg("epoch", 946684800ULL, 7258118399ULL, &ev)) {
    sendJsonError("invalid epoch");
    return;
  }

  int tzInt = 0;
  bool hasTz = false;
  if (!parseOptIntArg("tzOffset", -840, 840, &hasTz, &tzInt)) {
    sendJsonError("invalid timezone");
    return;
  }

  if (!setDS3231Epoch((uint64_t)ev)) {
    sendJsonError("rtc write failed");
    return;
  }

  if (hasTz && (int16_t)tzInt != cfg.tzOffsetMinutes) {
    cfg.tzOffsetMinutes = (int16_t)tzInt;
    requestConfigSave(true);
  }

  updateRTC();
  updateRTCAux();
  sendJsonOk();
}

static void handleRtcSet() {
  long year;
  long month;
  long day;
  long hour;
  long minute;
  long second;

  if (!parseLongArg("year", 2000, 2199, &year, true) ||
      !parseLongArg("month", 1, 12, &month, true) ||
      !parseLongArg("day", 1, 31, &day, true) ||
      !parseLongArg("hour", 0, 23, &hour, true) ||
      !parseLongArg("minute", 0, 59, &minute, true) ||
      !parseLongArg("second", 0, 59, &second, true)) {
    sendJsonError("invalid date/time");
    return;
  }

  if (day > daysInMonth((int)year, (uint8_t)month)) {
    sendJsonError("invalid day");
    return;
  }

  uint64_t e = partsToEpoch((int)year, (int)month, (int)day, (int)hour, (int)minute, (int)second);
  if (!setDS3231Epoch(e)) {
    sendJsonError("rtc write failed");
    return;
  }

  updateRTC();
  updateRTCAux();
  sendJsonOk();
}

static void handleRtcControl() {
  int agingInt = 0;
  int controlInt = 0;
  int clearOsfInt = 0;
  int en32kInt = 0;
  int convtInt = 0;

  bool hasAging = false;
  bool hasControl = false;
  bool hasClearOsf = false;
  bool hasEn32k = false;
  bool hasConvT = false;

  if (!parseOptIntArg("aging", -128, 127, &hasAging, &agingInt)) {
    sendJsonError("invalid aging");
    return;
  }
  if (!parseOptIntArg("control", 0, 255, &hasControl, &controlInt)) {
    sendJsonError("invalid control");
    return;
  }
  if (!parseOptIntArg("clearosf", 0, 1, &hasClearOsf, &clearOsfInt)) {
    sendJsonError("invalid clearosf");
    return;
  }
  if (!parseOptIntArg("en32k", 0, 1, &hasEn32k, &en32kInt)) {
    sendJsonError("invalid en32k");
    return;
  }
  if (!parseOptIntArg("convt", 0, 1, &hasConvT, &convtInt)) {
    sendJsonError("invalid convt");
    return;
  }

  bool needStatusWrite = hasClearOsf || hasEn32k;
  uint8_t status = 0;
  if (needStatusWrite) {
    if (!i2cReadByte(0x0F, &status)) {
      sendJsonError("rtc bus error");
      return;
    }
    if (hasClearOsf && clearOsfInt) status &= (uint8_t)~0x80;
    if (hasEn32k) {
      if (en32kInt) status |= 0x08;
      else status &= (uint8_t)~0x08;
    }
  }

  bool needControlWrite = hasControl || (hasConvT && convtInt);
  uint8_t control = 0;
  if (hasControl) {
    control = (uint8_t)controlInt;
  }
  if (hasConvT && convtInt) {
    if (!hasControl) {
      if (!i2cReadByte(0x0E, &control)) {
        sendJsonError("rtc bus error");
        return;
      }
    }
    control |= 0x20;
  }

  if (hasAging) {
    if (!i2cWriteByte(0x10, (uint8_t)(int8_t)agingInt)) {
      sendJsonError("rtc write failed");
      return;
    }
  }

  if (needStatusWrite) {
    if (!i2cWriteByte(0x0F, status)) {
      sendJsonError("rtc write failed");
      return;
    }
  }

  if (needControlWrite) {
    if (!i2cWriteByte(0x0E, control)) {
      sendJsonError("rtc write failed");
      return;
    }
  }

  updateRTCAux();
  sendJsonOk();
}

static bool parseAlarmRequest(Alarm& a, const char** err) {
  long hour;
  long minute;
  long days;
  long duration;

  if (!parseLongArg("hour", 0, 23, &hour, true)) {
    *err = "invalid hour";
    return false;
  }
  if (!parseLongArg("minute", 0, 59, &minute, true)) {
    *err = "invalid minute";
    return false;
  }
  if (!parseLongArg("days", 0, 127, &days, true)) {
    *err = "invalid days";
    return false;
  }
  if (!parseLongArg("duration", 1, 65535, &duration, true)) {
    *err = "invalid duration";
    return false;
  }

  String en = server.arg("enabled");
  bool enabled = (en == "1" || en == "true" || en == "on");

  String name = server.arg("name");
  if (name.length() != strlen(name.c_str())) {
    *err = "invalid name";
    return false;
  }

  memset(&a, 0, sizeof(a));
  a.enabled = enabled ? 1 : 0;
  a.hour = (uint8_t)hour;
  a.minute = (uint8_t)minute;
  a.daysMask = (uint8_t)days;
  a.durationSec = (uint16_t)duration;

  copySanitizedString(a.name, sizeof(a.name), name.c_str());
  if (a.name[0] == 0) {
    copyStringSafe(a.name, sizeof(a.name), "Alarm");
  }

  if (!isTextString(a.name, sizeof(a.name))) {
    *err = "invalid name";
    return false;
  }

  return true;
}

static void handleAlarmsSet() {
  String action = server.arg("action");
  if (action.length() != strlen(action.c_str())) {
    sendJsonError("invalid action");
    return;
  }

  if (action == "delete" || action == "toggle") {
    long idx;
    if (!parseLongArg("index", 0, MAX_ALARMS - 1, &idx, true)) {
      sendJsonError("invalid index");
      return;
    }
    int i = (int)idx;
    if (i >= cfg.alarmCount) {
      sendJsonError("invalid index");
      return;
    }

    if (action == "delete") {
      if (alarmActiveIndex == i) {
        stopAlarmOutput();
      } else if (alarmActiveIndex > i) {
        alarmActiveIndex--;
      }
      for (int j = i; j < (int)cfg.alarmCount - 1; j++) {
        cfg.alarms[j] = cfg.alarms[j + 1];
      }
      cfg.alarmCount--;
      memset(&cfg.alarms[cfg.alarmCount], 0, sizeof(Alarm));
    } else {
      cfg.alarms[i].enabled = cfg.alarms[i].enabled ? 0 : 1;
    }
  } else if (action == "add" || action == "update") {
    Alarm a;
    const char* err = nullptr;
    if (!parseAlarmRequest(a, &err)) {
      sendJsonError(err);
      return;
    }

    if (action == "add") {
      if (cfg.alarmCount >= MAX_ALARMS) {
        sendJsonError("alarm limit");
        return;
      }
      cfg.alarms[cfg.alarmCount++] = a;
    } else {
      long idx;
      if (!parseLongArg("index", 0, MAX_ALARMS - 1, &idx, true)) {
        sendJsonError("invalid index");
        return;
      }
      int i = (int)idx;
      if (i >= cfg.alarmCount) {
        sendJsonError("invalid index");
        return;
      }
      cfg.alarms[i] = a;
    }
  } else {
    sendJsonError("invalid action");
    return;
  }

  requestConfigSave();
  reprimeAlarms();
  sendJsonOk();
}

static void handleAlarmStop() {
  stopAlarmOutput();
  sendJsonOk();
}

static void handleAlarmTest() {
  triggerAlarmOutput(5, -1, nowMs64());
  sendJsonOk();
}

static void handleWifiSet() {
  if (!server.hasArg("ssid") || !server.hasArg("pass") || !server.hasArg("channel") || !server.hasArg("maxConn")) {
    sendJsonError("missing fields");
    return;
  }

  String ssidS = server.arg("ssid");
  String passS = server.arg("pass");
  if (ssidS.length() != strlen(ssidS.c_str()) || passS.length() != strlen(passS.c_str())) {
    sendJsonError("invalid string");
    return;
  }

  char newSsid[33];
  char newPass[64];
  copyStringSafe(newSsid, sizeof(newSsid), ssidS.c_str());
  copyStringSafe(newPass, sizeof(newPass), passS.c_str());

  if (!validSSID(newSsid)) {
    sendJsonError("invalid ssid");
    return;
  }
  if (!validPass(newPass)) {
    sendJsonError("invalid password");
    return;
  }

  long channel;
  long maxConn;
  if (!parseLongArg("channel", 1, 13, &channel, true)) {
    sendJsonError("invalid channel");
    return;
  }
  if (!parseLongArg("maxConn", 1, 8, &maxConn, true)) {
    sendJsonError("invalid max connections");
    return;
  }

  bool hidden = false;
  if (server.hasArg("hidden")) {
    String hs = server.arg("hidden");
    if (hs == "1" || hs == "true" || hs == "on") hidden = true;
  }

  Config backup = cfg;
  copyStringSafe(cfg.apSsid, sizeof(cfg.apSsid), newSsid);
  copyStringSafe(cfg.apPass, sizeof(cfg.apPass), newPass);
  cfg.apChannel = (uint8_t)channel;
  cfg.apHidden = hidden ? 1 : 0;
  cfg.apMaxConn = (uint8_t)maxConn;

  if (!saveConfigToNVS()) {
    cfg = backup;
    sendJsonError("config save failed");
    return;
  }

  configDirty = false;
  configForceSave = false;
  scheduleAPApply();
  sendJsonOk();
}

static void handleWifiRestart() {
  scheduleAPApply();
  sendJsonOk();
}

static void handleFactoryResetRequest() {
  factoryResetScheduled = true;
  factoryResetScheduledAtMs = nowMs64() + 1000ULL;
  sendJsonOk();
}

static void handleNotFound() {
  if (server.uri().startsWith("/api/")) {
    noCacheHeaders();
    sendBody(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
    return;
  }

  IPAddress ip = WiFi.softAPIP();
  char url[40];
  snprintf(url, sizeof(url), "http://%u.%u.%u.%u/", (unsigned)ip[0], (unsigned)ip[1], (unsigned)ip[2], (unsigned)ip[3]);

  noCacheHeaders();
  server.sendHeader("Location", url, true);
  server.send(302, "text/plain", "");
}

void setup() {
  esp_log_level_set("*", ESP_LOG_NONE);

  pinMode(FACTORY_RESET_PIN, INPUT_PULLUP);
  Wire.begin(SDA_PIN, SCL_PIN, 100000UL);
  Wire.setClock(100000UL);

  loadConfig();

  pinMode(ALARM_PIN, OUTPUT);
  stopAlarmOutput();

  WiFi.persistent(false);
  startAPFromConfig();

  server.on("/", HTTP_GET, handleIndex);
  server.on("/ds3231", HTTP_GET, handleIndex);
  server.on("/wifi", HTTP_GET, handleIndex);
  server.on("/style.css", HTTP_GET, handleCss);
  server.on("/app.js", HTTP_GET, handleJs);

  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/time/sync", HTTP_POST, handleSyncTime);
  server.on("/api/rtc/set", HTTP_POST, handleRtcSet);
  server.on("/api/rtc/control", HTTP_POST, handleRtcControl);
  server.on("/api/alarms/set", HTTP_POST, handleAlarmsSet);
  server.on("/api/alarm/stop", HTTP_POST, handleAlarmStop);
  server.on("/api/alarm/test", HTTP_POST, handleAlarmTest);
  server.on("/api/wifi/set", HTTP_POST, handleWifiSet);
  server.on("/api/wifi/restart", HTTP_POST, handleWifiRestart);
  server.on("/api/system/factoryreset", HTTP_POST, handleFactoryResetRequest);

  server.onNotFound(handleNotFound);
  server.begin();

  updateRTC();
  updateRTCAux();
}

void loop() {
  uint64_t now = nowMs64();

  pollFactoryButton(now);
  processConfigSave(now);
  processScheduledTasks(now);

  if (now - lastRtcReadMs >= RTC_READ_INTERVAL_MS) {
    updateRTC();
  }

  if (now - lastRtcAuxReadMs >= RTC_AUX_INTERVAL_MS) {
    updateRTCAux();
  }

  if (alarmOutputActive && now >= alarmOutputOffAtMs) {
    stopAlarmOutput();
  }

  dnsServer.processNextRequest();
  server.handleClient();
}

const char APP_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>ESP32 Alarm Clock</title>
<link rel="stylesheet" href="/style.css">
</head>
<body>
<header class="topbar">
  <div class="brand"><span class="dot"></span> ESP32 Alarm Clock</div>
  <nav id="nav">
    <a href="/" data-route="/">Dashboard</a>
    <a href="/ds3231" data-route="/ds3231">DS3231</a>
    <a href="/wifi" data-route="/wifi">Wi-Fi AP</a>
  </nav>
</header>
<main>
<section id="page-dash" class="page">
  <div class="hero card">
    <div class="clock" id="clock">--:--:--</div>
    <div class="date" id="date">Loading...</div>
    <div class="hero-actions">
      <button id="btnSync" class="btn primary">Sync Browser Time</button>
      <button id="btnStop" class="btn danger">Stop Alarm</button>
      <button id="btnTest" class="btn">Test Alarm</button>
    </div>
  </div>
  <div class="grid stats">
    <div class="card stat"><span>RTC</span><strong id="statRtc">--</strong></div>
    <div class="card stat"><span>Temperature</span><strong id="statTemp">--</strong></div>
    <div class="card stat"><span>Alarm Output</span><strong id="statOut">--</strong></div>
    <div class="card stat"><span>AP Clients</span><strong id="statClients">--</strong></div>
    <div class="card stat"><span>Heap</span><strong id="statHeap">--</strong></div>
    <div class="card stat"><span>Uptime</span><strong id="statUptime">--</strong></div>
  </div>
  <div class="grid two">
    <div class="card">
      <div class="card-head">
        <h2>Calendar</h2>
        <div class="calendar-nav">
          <button id="calPrev" class="btn small">‹</button>
          <span id="calLabel"></span>
          <button id="calNext" class="btn small">›</button>
        </div>
      </div>
      <div class="calendar-head">
        <span>Sun</span><span>Mon</span><span>Tue</span><span>Wed</span><span>Thu</span><span>Fri</span><span>Sat</span>
      </div>
      <div id="calendar" class="calendar-grid"></div>
    </div>
    <div class="card">
      <div class="card-head"><h2>Alarm Schedule</h2></div>
      <div id="alarmList" class="alarm-list"></div>
    </div>
  </div>
  <div class="card">
    <div class="card-head"><h2 id="alarmFormTitle">New Alarm</h2></div>
    <form id="alarmForm" class="form-grid" autocomplete="off">
      <input type="hidden" name="index" value="-1">
      <label class="switch"><input type="checkbox" name="enabled" checked> Enabled</label>
      <label>Name<input type="text" name="name" maxlength="19" placeholder="Alarm"></label>
      <label>Time<input type="time" name="time" required></label>
      <label>Duration (seconds)<input type="number" name="duration" min="1" max="65535" value="60"></label>
      <div class="days">
        <label><input type="checkbox" name="day0"> Sun</label>
        <label><input type="checkbox" name="day1"> Mon</label>
        <label><input type="checkbox" name="day2"> Tue</label>
        <label><input type="checkbox" name="day3"> Wed</label>
        <label><input type="checkbox" name="day4"> Thu</label>
        <label><input type="checkbox" name="day5"> Fri</label>
        <label><input type="checkbox" name="day6"> Sat</label>
      </div>
      <div class="form-actions">
        <button type="submit" class="btn primary">Save Alarm</button>
        <button type="button" id="alarmNew" class="btn">New</button>
      </div>
    </form>
  </div>
</section>

<section id="page-rtc" class="page">
  <div class="card">
    <div class="card-head"><h2>DS3231 Real Time Clock</h2><span id="rtcBadge" class="badge">--</span></div>
    <div class="grid stats">
      <div class="stat"><span>Local Time</span><strong id="rtcLocal">--</strong></div>
      <div class="stat"><span>UTC Time</span><strong id="rtcUtc">--</strong></div>
      <div class="stat"><span>Temperature</span><strong id="rtcTemp">--</strong></div>
      <div class="stat"><span>Status</span><strong id="rtcStatus">--</strong></div>
      <div class="stat"><span>Control</span><strong id="rtcControl">--</strong></div>
      <div class="stat"><span>Aging</span><strong id="rtcAging">--</strong></div>
    </div>
    <div class="actions">
      <button id="rtcSync" class="btn primary">Sync Browser Time</button>
      <button id="rtcUseNow" class="btn">Use Current UTC</button>
    </div>
  </div>
  <div class="grid two">
    <div class="card">
      <div class="card-head"><h2>Set Time (UTC)</h2></div>
      <form id="rtcSetForm" class="form-grid">
        <label>Year<input type="number" name="year" min="2000" max="2199" required></label>
        <label>Month<input type="number" name="month" min="1" max="12" required></label>
        <label>Day<input type="number" name="day" min="1" max="31" required></label>
        <label>Hour<input type="number" name="hour" min="0" max="23" required></label>
        <label>Minute<input type="number" name="minute" min="0" max="59" required></label>
        <label>Second<input type="number" name="second" min="0" max="59" required></label>
        <div class="form-actions"><button class="btn primary" type="submit">Write RTC</button></div>
      </form>
    </div>
    <div class="card">
      <div class="card-head"><h2>Register Controls</h2></div>
      <div class="actions wrap">
        <button id="rtcClearOsf" class="btn">Clear OSF</button>
        <button id="rtcConvTemp" class="btn">Force Temp Conversion</button>
        <button id="rtcToggle32k" class="btn">Toggle 32kHz</button>
      </div>
      <form id="rtcControlForm" class="form-grid">
        <label>Aging Offset (-128..127)<input type="number" name="aging" min="-128" max="127" value="0"></label>
        <label>Control Register (0..255)<input type="number" name="control" min="0" max="255" value="0"></label>
        <div class="form-actions"><button class="btn primary" type="submit">Write Registers</button></div>
      </form>
    </div>
  </div>
</section>

<section id="page-wifi" class="page">
  <div class="card">
    <div class="card-head"><h2>Wi-Fi Access Point</h2><span id="wifiBadge" class="badge">AP</span></div>
    <div class="grid stats">
      <div class="stat"><span>SSID</span><strong id="wifiSsid">--</strong></div>
      <div class="stat"><span>IP Address</span><strong id="wifiIp">--</strong></div>
      <div class="stat"><span>Connected Clients</span><strong id="wifiClients">--</strong></div>
      <div class="stat"><span>Channel</span><strong id="wifiChannel">--</strong></div>
      <div class="stat"><span>Hidden</span><strong id="wifiHidden">--</strong></div>
      <div class="stat"><span>Max Connections</span><strong id="wifiMax">--</strong></div>
    </div>
  </div>
  <div class="card">
    <div class="card-head"><h2>Access Point Settings</h2></div>
    <form id="wifiForm" class="form-grid">
      <label>SSID<input type="text" name="ssid" maxlength="32" required></label>
      <label>Password (8-63 chars or empty)<input type="password" name="pass" maxlength="63"></label>
      <label>Channel<input type="number" name="channel" min="1" max="13" required></label>
      <label>Max Connections<input type="number" name="maxConn" min="1" max="8" required></label>
      <label class="switch wide"><input type="checkbox" name="hidden"> Hidden SSID</label>
      <div class="form-actions wide">
        <button class="btn primary" type="submit">Save and Restart AP</button>
        <button type="button" id="wifiRestart" class="btn">Restart AP</button>
      </div>
    </form>
    <p class="hint">Changes restart the access point. Reconnect to the new SSID if changed.</p>
  </div>
  <div class="card danger-zone">
    <div class="card-head"><h2>Factory Reset</h2></div>
    <p class="hint">Hold GPIO0 for 5 seconds or use this button to erase configuration and restart.</p>
    <button id="factoryReset" class="btn danger">Factory Reset Device</button>
  </div>
</section>
</main>
<div id="toast" class="toast"></div>
<script src="/app.js"></script>
</body>
</html>)HTML";

const char STYLE_CSS[] PROGMEM = R"CSS(:root{
  --bg:#09111f;
  --bg2:#0f1b31;
  --card:#121f37;
  --card2:#172744;
  --text:#eaf1ff;
  --muted:#9fb1cf;
  --line:#25395c;
  --accent:#56a8ff;
  --accent2:#7ee0b8;
  --danger:#ff6b7a;
  --warn:#ffcc66;
  --ok:#65e6a4;
  --shadow:0 18px 50px rgba(0,0,0,.35);
}
*{box-sizing:border-box}
html,body{margin:0;min-height:100%}
body{
  font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;
  color:var(--text);
  background:
    radial-gradient(1200px 700px at 15% -10%,rgba(86,168,255,.24),transparent 60%),
    radial-gradient(900px 600px at 100% 0%,rgba(126,224,184,.14),transparent 55%),
    linear-gradient(180deg,var(--bg),var(--bg2));
  min-height:100vh;
}
a{color:inherit;text-decoration:none}
button,input{font:inherit}
.topbar{
  position:sticky;top:0;z-index:20;
  display:flex;align-items:center;justify-content:space-between;gap:1rem;
  padding:.8rem 1rem;
  background:rgba(9,17,31,.82);
  border-bottom:1px solid var(--line);
  backdrop-filter:blur(14px);
}
.brand{font-weight:800;letter-spacing:.2px;display:flex;align-items:center;gap:.6rem}
.brand .dot{
  width:.7rem;height:.7rem;border-radius:50%;
  background:var(--accent2);
  box-shadow:0 0 16px var(--accent2);
}
nav{display:flex;gap:.35rem;flex-wrap:wrap}
nav a{
  padding:.55rem .8rem;border-radius:999px;color:var(--muted);
  border:1px solid transparent;transition:.18s ease;
  white-space:nowrap;
}
nav a:hover{color:var(--text);background:rgba(255,255,255,.05)}
nav a.active{color:var(--text);border-color:var(--line);background:rgba(86,168,255,.16)}
main{width:min(1320px,100%);margin:0 auto;padding:1rem}
.page{display:none}
.page.active{display:block;animation:fade .2s ease}
@keyframes fade{from{opacity:0;transform:translateY(4px)}to{opacity:1;transform:none}}
.card{
  background:linear-gradient(180deg,rgba(23,39,68,.94),rgba(18,31,55,.94));
  border:1px solid var(--line);
  border-radius:18px;
  padding:1rem;
  box-shadow:var(--shadow);
  margin-bottom:1rem;
}
.hero{
  text-align:center;
  padding:clamp(1.2rem,4vw,2.4rem);
  overflow:hidden;
  position:relative;
}
.hero:after{
  content:"";
  position:absolute;inset:-40% -10% auto;
  height:220px;
  background:radial-gradient(circle,rgba(86,168,255,.18),transparent 60%);
  pointer-events:none;
}
.clock{
  position:relative;z-index:1;
  font-size:clamp(2.3rem,8vw,5rem);
  line-height:1.05;
  font-weight:850;
  letter-spacing:.03em;
  text-shadow:0 0 30px rgba(86,168,255,.22);
}
.date{
  position:relative;z-index:1;
  color:var(--muted);
  font-size:clamp(.95rem,2.4vw,1.2rem);
  margin-top:.4rem;
  letter-spacing:.04em;
}
.hero-actions{
  position:relative;z-index:1;
  display:flex;gap:.6rem;justify-content:center;flex-wrap:wrap;
  margin-top:1.1rem;
}
.grid{display:grid;gap:1rem}
.grid.stats{grid-template-columns:repeat(auto-fit,minmax(150px,1fr))}
.grid.two{grid-template-columns:repeat(auto-fit,minmax(300px,1fr))}
.card.stat{padding:.95rem;margin:0;border-radius:14px}
.stat span{display:block;color:var(--muted);font-size:.8rem;text-transform:uppercase;letter-spacing:.08em}
.stat strong{display:block;margin-top:.35rem;font-size:1.2rem;word-break:break-word}
.card-head{
  display:flex;align-items:center;justify-content:space-between;gap:1rem;
  margin-bottom:.9rem;
}
.card-head h2{font-size:1.05rem;margin:0}
.calendar-nav{display:flex;align-items:center;gap:.55rem;color:var(--muted)}
.calendar-head,.calendar-grid{display:grid;grid-template-columns:repeat(7,minmax(0,1fr));gap:.45rem}
.calendar-head span{
  text-align:center;color:var(--muted);
  font-size:.72rem;text-transform:uppercase;letter-spacing:.08em;
}
.calendar-grid{margin-top:.6rem}
.cal-cell{
  min-height:54px;border-radius:12px;border:1px solid rgba(37,57,92,.75);
  background:rgba(10,20,37,.35);
  padding:.4rem;position:relative;
  font-weight:650;
}
.cal-cell.empty{opacity:.25}
.cal-cell.today{border-color:var(--accent);box-shadow:inset 0 0 0 1px var(--accent)}
.cal-cell.has-alarm:after{
  content:"";position:absolute;right:.4rem;bottom:.4rem;
  width:.42rem;height:.42rem;border-radius:50%;
  background:var(--warn);box-shadow:0 0 10px var(--warn);
}
.alarm-list{display:flex;flex-direction:column;gap:.65rem}
.empty{color:var(--muted);padding:.5rem 0}
.alarm-item{
  display:flex;align-items:center;justify-content:space-between;gap:.75rem;
  padding:.75rem;border:1px solid var(--line);border-radius:14px;
  background:rgba(10,20,37,.32);
}
.alarm-item.disabled{opacity:.55}
.alarm-main{display:flex;align-items:center;gap:.8rem;min-width:0}
.alarm-time{font-size:1.45rem;font-weight:850;white-space:nowrap}
.alarm-meta{min-width:0}
.alarm-meta strong{display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.alarm-meta span{display:block;color:var(--muted);font-size:.8rem;margin-top:.15rem}
.alarm-actions{display:flex;gap:.4rem;flex-wrap:wrap;justify-content:flex-end}
.btn{
  appearance:none;border:1px solid var(--line);
  background:rgba(255,255,255,.035);
  color:var(--text);
  padding:.62rem .9rem;border-radius:12px;cursor:pointer;
  transition:.16s ease;font-weight:650;
}
.btn:hover{background:rgba(255,255,255,.08);transform:translateY(-1px)}
.btn:active{transform:translateY(0)}
.btn.primary{background:linear-gradient(135deg,#3d8cff,#56a8ff);border-color:transparent;color:#fff}
.btn.danger{background:rgba(255,107,122,.13);border-color:rgba(255,107,122,.45);color:#ffb8c0}
.btn.small{padding:.42rem .65rem;border-radius:10px;font-size:.82rem}
.btn.danger:hover{background:rgba(255,107,122,.2)}
.form-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:.8rem;align-items:end}
label{display:flex;flex-direction:column;gap:.35rem;color:var(--muted);font-size:.85rem}
label.wide,.wide{grid-column:1/-1}
input[type="text"],input[type="password"],input[type="number"],input[type="time"]{
  width:100%;
  border:1px solid var(--line);
  border-radius:12px;
  background:rgba(5,12,23,.56);
  color:var(--text);
  padding:.7rem .75rem;
  outline:none;
}
input:focus{border-color:var(--accent);box-shadow:0 0 0 3px rgba(86,168,255,.13)}
.switch{flex-direction:row;align-items:center;gap:.55rem;color:var(--text);font-size:.9rem}
.switch input{width:1.1rem;height:1.1rem;accent-color:var(--accent)}
.days{display:flex;gap:.5rem;flex-wrap:wrap;grid-column:1/-1}
.days label{
  flex-direction:row;align-items:center;gap:.35rem;color:var(--text);
  border:1px solid var(--line);border-radius:999px;padding:.42rem .62rem;
  background:rgba(255,255,255,.025);
}
.days input{accent-color:var(--accent)}
.form-actions{display:flex;gap:.65rem;flex-wrap:wrap;align-items:center}
.actions{display:flex;gap:.65rem;flex-wrap:wrap;margin-top:.9rem}
.actions.wrap{flex-wrap:wrap}
.hint{color:var(--muted);font-size:.88rem;line-height:1.55;margin:.8rem 0 0}
.badge{
  display:inline-flex;align-items:center;justify-content:center;
  min-height:1.7rem;padding:.2rem .65rem;border-radius:999px;
  border:1px solid var(--line);color:var(--muted);font-size:.78rem;font-weight:750;
}
.badge.ok{color:var(--ok);border-color:rgba(101,230,164,.4);background:rgba(101,230,164,.1)}
.badge.warn{color:var(--warn);border-color:rgba(255,204,102,.4);background:rgba(255,204,102,.1)}
.badge.bad{color:var(--danger);border-color:rgba(255,107,122,.4);background:rgba(255,107,122,.1)}
.danger-zone{border-color:rgba(255,107,122,.3)}
.toast{
  position:fixed;left:50%;bottom:1.1rem;z-index:100;
  transform:translate(-50%,20px);
  min-width:min(92vw,340px);max-width:92vw;
  padding:.8rem 1rem;border-radius:14px;
  background:rgba(15,27,49,.96);border:1px solid var(--line);
  color:var(--text);box-shadow:var(--shadow);
  opacity:0;pointer-events:none;transition:.2s ease;
  text-align:center;font-weight:650;
}
.toast.show{opacity:1;transform:translate(-50%,0)}
.toast.error{border-color:rgba(255,107,122,.55);color:#ffd7dc}
@media (max-width:640px){
  .topbar{align-items:flex-start;flex-direction:column}
  main{padding:.75rem}
  .card{border-radius:15px;padding:.85rem}
  .alarm-item{align-items:flex-start;flex-direction:column}
  .alarm-actions{width:100%;justify-content:flex-start}
  .calendar-head,.calendar-grid{gap:.3rem}
  .cal-cell{min-height:44px;border-radius:10px;padding:.3rem;font-size:.85rem}
  .grid.two{grid-template-columns:1fr}
}
)CSS";

const char APP_JS[] PROGMEM = R"JS((function(){
'use strict';

const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => Array.from(r.querySelectorAll(s));
const DAYS = ['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];

let state = null;
let calYear = null;
let calMonth = null;
let calSig = '';
let autoSynced = false;
let rtcFormFilled = false;
let wifiFormFilled = false;
let alarmSig = '';
let activePage = 'dash';

function pad(n){ return String(n).padStart(2,'0'); }
function esc(s){
  return String(s == null ? '' : s).replace(/[&<>"']/g, c => ({
    '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'
  }[c]));
}
function setText(id, v){
  const el = $('#' + id);
  if (el) el.textContent = v == null ? '--' : String(v);
}
function toast(msg, err){
  const t = $('#toast');
  if (!t) return;
  t.textContent = msg;
  t.className = 'toast show' + (err ? ' error' : '');
  clearTimeout(t._tm);
  t._tm = setTimeout(() => { t.className = 'toast'; }, 2600);
}
function on(id, ev, fn){
  const el = $('#' + id);
  if (el) el.addEventListener(ev, fn);
}
async function fetchJson(url, opt){
  const r = await fetch(url, Object.assign({ cache: 'no-store' }, opt || {}));
  if (!r.ok) throw new Error('HTTP ' + r.status);
  const text = await r.text();
  return text ? JSON.parse(text) : {};
}
function postForm(url, data){
  const b = new URLSearchParams();
  for (const k in data) {
    if (data[k] !== undefined && data[k] !== null) b.append(k, String(data[k]));
  }
  return fetchJson(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
    body: b.toString()
  });
}
function fmtUptime(sec){
  sec = Number(sec) || 0;
  const d = Math.floor(sec / 86400);
  sec %= 86400;
  const h = Math.floor(sec / 3600);
  sec %= 3600;
  const m = Math.floor(sec / 60);
  const s = sec % 60;
  let out = '';
  if (d) out += d + 'd ';
  out += pad(h) + ':' + pad(m) + ':' + pad(s);
  return out;
}
function setPage(){
  const p = location.pathname;
  activePage = p.startsWith('/ds3231') ? 'rtc' : p.startsWith('/wifi') ? 'wifi' : 'dash';
  $$('.page').forEach(el => el.classList.toggle('active', el.id === 'page-' + activePage));
  $$('#nav a').forEach(a => {
    const r = a.getAttribute('data-route');
    a.classList.toggle('active',
      (activePage === 'dash' && r === '/') ||
      (activePage === 'rtc' && r === '/ds3231') ||
      (activePage === 'wifi' && r === '/wifi')
    );
  });
}
function initCalendar(s){
  if (!s || !s.rtcValid) return;
  if (calYear === null) {
    calYear = s.year;
    calMonth = s.month;
  }
}
function renderCalendar(s){
  if (!s || calYear === null) return;
  const y = calYear;
  const m = calMonth;
  const sig = y + '-' + m + '-' + (s.rtcValid ? 1 : 0) + '-' +
              (s.rtcValid ? (s.year + '-' + s.month + '-' + s.day) : '') + '-' + alarmSig;
  if (sig === calSig) return;
  calSig = sig;

  const first = new Date(Date.UTC(y, m - 1, 1));
  const start = first.getUTCDay();
  const dim = new Date(Date.UTC(y, m, 0)).getUTCDate();
  const prevDim = new Date(Date.UTC(y, m - 1, 0)).getUTCDate();
  const cells = [];

  for (let i = start - 1; i >= 0; i--) {
    cells.push({ d: prevDim - i, other: true });
  }
  for (let d = 1; d <= dim; d++) {
    cells.push({ d, other: false });
  }
  let next = 1;
  while (cells.length % 7 !== 0) {
    cells.push({ d: next++, other: true });
  }

  const label = first.toLocaleDateString(undefined, { month: 'long', year: 'numeric', timeZone: 'UTC' });
  const lbl = $('#calLabel');
  if (lbl) lbl.textContent = label;

  const cal = $('#calendar');
  if (!cal) return;

  cal.innerHTML = cells.map(cell => {
    let cls = 'cal-cell';
    if (cell.other) cls += ' empty';
    if (!cell.other && s.rtcValid && y === s.year && m === s.month && cell.d === s.day) cls += ' today';

    let hasAlarm = false;
    if (!cell.other && s.alarms) {
      const wd = new Date(Date.UTC(y, m - 1, cell.d)).getUTCDay();
      hasAlarm = s.alarms.some(a => a.enabled && (a.daysMask & (1 << wd)));
    }
    if (hasAlarm) cls += ' has-alarm';

    return '<div class="' + cls + '">' + cell.d + '</div>';
  }).join('');
}
function renderAlarms(s){
  const list = $('#alarmList');
  if (!list) return;

  const sig = JSON.stringify(s.alarms || []);
  if (sig === alarmSig) return;
  alarmSig = sig;

  if (!s.alarms || s.alarms.length === 0) {
    list.innerHTML = '<div class="empty">No alarms configured.</div>';
    return;
  }

  list.innerHTML = s.alarms.map(a => {
    const days = DAYS.map((nm, i) => (a.daysMask & (1 << i)) ? nm : '').filter(Boolean).join(' ');
    return '<div class="alarm-item ' + (a.enabled ? '' : 'disabled') + '">' +
      '<div class="alarm-main">' +
        '<div class="alarm-time">' + pad(a.hour) + ':' + pad(a.minute) + '</div>' +
        '<div class="alarm-meta"><strong>' + esc(a.name || 'Alarm') + '</strong>' +
        '<span>' + a.duration + 's · ' + (days || 'Never') + '</span></div>' +
      '</div>' +
      '<div class="alarm-actions">' +
        '<button class="btn small" data-edit="' + a.index + '">Edit</button>' +
        '<button class="btn small" data-toggle="' + a.index + '">' + (a.enabled ? 'Disable' : 'Enable') + '</button>' +
        '<button class="btn small danger" data-del="' + a.index + '">Delete</button>' +
      '</div>' +
    '</div>';
  }).join('');
}
function editAlarm(index){
  if (!state || !state.alarms) return;
  const a = state.alarms.find(x => Number(x.index) === Number(index));
  if (!a) return;
  const f = $('#alarmForm');
  if (!f) return;

  f.elements.index.value = String(index);
  f.elements.enabled.checked = !!a.enabled;
  f.elements.name.value = a.name || '';
  f.elements.time.value = pad(a.hour) + ':' + pad(a.minute);
  f.elements.duration.value = a.duration;

  for (let i = 0; i < 7; i++) {
    f.elements['day' + i].checked = !!(a.daysMask & (1 << i));
  }

  const title = $('#alarmFormTitle');
  if (title) title.textContent = 'Edit Alarm';
  f.scrollIntoView({ behavior: 'smooth', block: 'center' });
}
function newAlarmForm(){
  const f = $('#alarmForm');
  if (!f) return;
  f.reset();
  f.elements.index.value = '-1';
  f.elements.enabled.checked = true;
  f.elements.duration.value = '60';
  for (let i = 0; i < 7; i++) f.elements['day' + i].checked = false;
  const title = $('#alarmFormTitle');
  if (title) title.textContent = 'New Alarm';
}
function renderRtc(s){
  setText('rtcLocal', s.timeStr);
  setText('rtcUtc', s.utcStr);
  setText('rtcTemp', s.temperature == null ? '--' : Number(s.temperature).toFixed(2) + ' °C');
  setText('rtcStatus', '0x' + Number(s.statusReg || 0).toString(16).padStart(2, '0'));
  setText('rtcControl', '0x' + Number(s.controlReg || 0).toString(16).padStart(2, '0'));
  setText('rtcAging', s.agingReg);
  setText('statRtc', s.rtcValid ? (s.osf ? 'OSF' : 'OK') : 'Invalid');
  setText('statTemp', s.temperature == null ? '--' : Number(s.temperature).toFixed(2) + ' °C');

  const b = $('#rtcBadge');
  if (b) {
    b.textContent = s.rtcValid ? (s.osf ? 'OSF SET' : 'OK') : 'NOT FOUND';
    b.className = 'badge ' + (s.rtcValid ? (s.osf ? 'warn' : 'ok') : 'bad');
  }

  if (!rtcFormFilled && s.rtcValid) {
    const f = $('#rtcSetForm');
    if (f) {
      f.elements.year.value = s.utcYear;
      f.elements.month.value = s.utcMonth;
      f.elements.day.value = s.utcDay;
      f.elements.hour.value = s.utcHour;
      f.elements.minute.value = s.utcMinute;
      f.elements.second.value = s.utcSecond;
    }
    const cf = $('#rtcControlForm');
    if (cf) {
      cf.elements.aging.value = s.agingReg;
      cf.elements.control.value = s.controlReg;
    }
    rtcFormFilled = true;
  }
}
function renderWifi(s){
  const ap = s.ap || {};
  setText('wifiSsid', ap.ssid);
  setText('wifiIp', ap.ip);
  setText('wifiClients', ap.clients);
  setText('wifiChannel', ap.channel);
  setText('wifiHidden', ap.hidden ? 'Yes' : 'No');
  setText('wifiMax', ap.maxConn);
  setText('statClients', ap.clients);

  if (!wifiFormFilled) {
    const f = $('#wifiForm');
    if (f) {
      f.elements.ssid.value = ap.ssid || '';
      f.elements.pass.value = ap.pass || '';
      f.elements.channel.value = ap.channel || 1;
      f.elements.maxConn.value = ap.maxConn || 4;
      f.elements.hidden.checked = !!ap.hidden;
    }
    wifiFormFilled = true;
  }
}
function renderStatus(s){
  state = s;

  setText('clock', s.clockStr);
  setText('date', s.dateStr);
  setText('statRtc', s.rtcValid ? (s.osf ? 'OSF' : 'OK') : 'Invalid');
  setText('statTemp', s.temperature == null ? '--' : Number(s.temperature).toFixed(2) + ' °C');
  setText('statOut', s.alarmOutput ? 'ACTIVE' : 'Off');
  setText('statHeap', Number(s.heap || 0).toLocaleString());
  setText('statUptime', fmtUptime(s.uptime));

  initCalendar(s);
  renderAlarms(s);
  renderCalendar(s);
  renderRtc(s);
  renderWifi(s);
}
async function refreshStatus(){
  const s = await fetchJson('/api/status');
  renderStatus(s);
}
async function syncBrowserTime(silent){
  const epoch = Math.floor(Date.now() / 1000);
  const tzOffset = -new Date().getTimezoneOffset();
  await postForm('/api/time/sync', { epoch: epoch, tzOffset: tzOffset });
  if (!silent) toast('DS3231 synced');
  rtcFormFilled = false;
  await refreshStatus();
}
function bindEvents(){
  const syncClick = () => syncBrowserTime(false).catch(e => toast(e.message, true));
  on('btnSync', 'click', syncClick);
  on('rtcSync', 'click', syncClick);

  on('btnStop', 'click', async () => {
    try { await postForm('/api/alarm/stop', {}); toast('Alarm stopped'); await refreshStatus(); }
    catch (e) { toast(e.message, true); }
  });

  on('btnTest', 'click', async () => {
    try { await postForm('/api/alarm/test', {}); toast('Test alarm started'); await refreshStatus(); }
    catch (e) { toast(e.message, true); }
  });

  on('calPrev', 'click', () => {
    if (!state) return;
    calMonth--;
    if (calMonth < 1) { calMonth = 12; calYear--; }
    renderCalendar(state);
  });

  on('calNext', 'click', () => {
    if (!state) return;
    calMonth++;
    if (calMonth > 12) { calMonth = 1; calYear++; }
    renderCalendar(state);
  });

  on('alarmNew', 'click', () => { newAlarmForm(); });

  const af = $('#alarmForm');
  if (af) {
    af.addEventListener('submit', async ev => {
      ev.preventDefault();
      const f = af;
      const time = String(f.elements.time.value || '');
      if (!/^\d{2}:\d{2}$/.test(time)) { toast('Select a time', true); return; }
      const parts = time.split(':').map(Number);
      let days = 0;
      for (let i = 0; i < 7; i++) {
        if (f.elements['day' + i].checked) days |= (1 << i);
      }
      const idx = f.elements.index.value;
      try {
        await postForm('/api/alarms/set', {
          action: idx === '-1' ? 'add' : 'update',
          index: idx,
          enabled: f.elements.enabled.checked ? 1 : 0,
          name: f.elements.name.value,
          hour: parts[0],
          minute: parts[1],
          days: days,
          duration: Number(f.elements.duration.value || 60)
        });
        toast('Alarm saved');
        alarmSig = '';
        await refreshStatus();
        newAlarmForm();
      } catch (e) { toast(e.message, true); }
    });
  }

  const al = $('#alarmList');
  if (al) {
    al.addEventListener('click', async ev => {
      const b = ev.target && ev.target.closest ? ev.target.closest('button') : null;
      if (!b) return;

      if (b.dataset.edit !== undefined) {
        editAlarm(Number(b.dataset.edit));
        return;
      }

      if (b.dataset.toggle !== undefined) {
        try {
          await postForm('/api/alarms/set', { action: 'toggle', index: b.dataset.toggle });
          alarmSig = '';
          await refreshStatus();
        } catch (e) { toast(e.message, true); }
        return;
      }

      if (b.dataset.del !== undefined) {
        if (!confirm('Delete this alarm?')) return;
        try {
          await postForm('/api/alarms/set', { action: 'delete', index: b.dataset.del });
          toast('Alarm deleted');
          alarmSig = '';
          await refreshStatus();
        } catch (e) { toast(e.message, true); }
      }
    });
  }

  on('rtcUseNow', 'click', () => {
    if (!state) return;
    const f = $('#rtcSetForm');
    if (!f) return;
    f.elements.year.value = state.utcYear;
    f.elements.month.value = state.utcMonth;
    f.elements.day.value = state.utcDay;
    f.elements.hour.value = state.utcHour;
    f.elements.minute.value = state.utcMinute;
    f.elements.second.value = state.utcSecond;
  });

  const rf = $('#rtcSetForm');
  if (rf) {
    rf.addEventListener('submit', async ev => {
      ev.preventDefault();
      try {
        await postForm('/api/rtc/set', {
          year: rf.elements.year.value,
          month: rf.elements.month.value,
          day: rf.elements.day.value,
          hour: rf.elements.hour.value,
          minute: rf.elements.minute.value,
          second: rf.elements.second.value
        });
        toast('RTC written');
        rtcFormFilled = false;
        await refreshStatus();
      } catch (e) { toast(e.message, true); }
    });
  }

  const cf = $('#rtcControlForm');
  if (cf) {
    cf.addEventListener('submit', async ev => {
      ev.preventDefault();
      try {
        await postForm('/api/rtc/control', {
          aging: cf.elements.aging.value,
          control: cf.elements.control.value
        });
        toast('Registers written');
        await refreshStatus();
      } catch (e) { toast(e.message, true); }
    });
  }

  on('rtcClearOsf', 'click', async () => {
    try { await postForm('/api/rtc/control', { clearosf: 1 }); toast('OSF cleared'); await refreshStatus(); }
    catch (e) { toast(e.message, true); }
  });

  on('rtcConvTemp', 'click', async () => {
    try { await postForm('/api/rtc/control', { convt: 1 }); toast('Temperature conversion requested'); await refreshStatus(); }
    catch (e) { toast(e.message, true); }
  });

  on('rtcToggle32k', 'click', async () => {
    if (!state) return;
    try {
      await postForm('/api/rtc/control', { en32k: state.en32k ? 0 : 1 });
      toast('32kHz toggled');
      await refreshStatus();
    } catch (e) { toast(e.message, true); }
  });

  const wf = $('#wifiForm');
  if (wf) {
    wf.addEventListener('submit', async ev => {
      ev.preventDefault();
      try {
        await postForm('/api/wifi/set', {
          ssid: wf.elements.ssid.value,
          pass: wf.elements.pass.value,
          channel: wf.elements.channel.value,
          maxConn: wf.elements.maxConn.value,
          hidden: wf.elements.hidden.checked ? 1 : 0
        });
        toast('Access point restarting');
        wifiFormFilled = false;
      } catch (e) { toast(e.message, true); }
    });
  }

  on('wifiRestart', 'click', async () => {
    try { await postForm('/api/wifi/restart', {}); toast('Access point restarting'); }
    catch (e) { toast(e.message, true); }
  });

  on('factoryReset', 'click', async () => {
    if (!confirm('Erase all settings and restart the device?')) return;
    try {
      await postForm('/api/system/factoryreset', {});
      toast('Factory reset scheduled');
    } catch (e) { toast(e.message, true); }
  });
}

async function boot(){
  setPage();
  bindEvents();

  try { await refreshStatus(); } catch (e) {}

  setInterval(() => { refreshStatus().catch(() => {}); }, 1000);

  setTimeout(async () => {
    if (autoSynced) return;
    autoSynced = true;
    try { await syncBrowserTime(true); } catch (e) {}
  }, 700);
}

if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', boot);
} else {
  boot();
}

})();)JS";
