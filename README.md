## 1. Hardware
- **ESP32** dev board
- **DS3231 RTC** module (optional, but recommended for timekeeping)
- **Alarm output**: GPIO 25 (active high)
- **Factory reset button**: GPIO 0 (hold 5 s)
- **I2C**: SDA = GPIO 21, SCL = GPIO 22

## 2. Install libraries (Arduino IDE)
- `RTClib` (Adafruit)
- `ESPAsyncWebServer` + `AsyncTCP`
- `DNSServer` (bundled with ESP32 core)
- `Preferences` (bundled with ESP32 core)

## 3. Flash
1. Open `ESP32_Alarm_Clock.ino` in Arduino IDE.
2. Select your ESP32 board and correct COM port.
3. Upload.

## 4. First use
1. On your phone/PC, connect to Wi-Fi **`ESP32_ALARM_CLOCK`** (password **`12345678`**).
2. A captive portal should open automatically; otherwise browse to **http://192.168.4.1**.
3. The page will prompt to **sync time from your browser** — accept it (or use *Time settings → Sync from this device*).

## 5. Main features
- **Overview** – live clock, next alarm, today's events
- **Alarms** – up to 12 weekly or one-time alarms (coincident alarms ring together)
- **Time settings** – sync from browser, UTC offset, 12/24-hour display
- **Calendar** – up to 24 dated events, week start & date format
- **Wi-Fi AP** – change SSID/password (device restarts the AP)

When an alarm rings, the web banner offers **Snooze 5 min** or **Dismiss**. The GPIO 25 output mirrors the ringing state.

## 6. Reset / recovery
- **Hold GPIO 0 for 5 seconds** → factory reset (defaults restored, device restarts).
- Or POST to `/api/factory-reset` from the web UI.
- Default AP credentials are restored: `ESP32_ALARM_CLOCK` / `12345678`.

## 7. Notes
- Settings are saved to flash with a debounce (~5 s) and a minimum write interval (60 s); a restart may be delayed until the save completes.
- If no DS3231 is detected, the ESP32 internal clock is used (time is lost on power-off until re-synced).
- Keep the browser page open during time sync so the timestamp arrives correctly.
