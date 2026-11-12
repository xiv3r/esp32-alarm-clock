## 1. Hardware Required

| Component | Details |
|---|---|
| ESP32 dev board | Any standard ESP32 (WROOM/WROVER) |
| DS3231 RTC module | I2C, includes battery backup |
| Relay module | Connected to GPIO 25 (alarm output) |
| Boot button | On-board GPIO 0 (built-in BOOT button) |

---

## 2. Wiring

| Signal | ESP32 Pin | DS3231 / Relay |
|---|---|---|
| SDA | GPIO 21 | DS3231 SDA |
| SCL | GPIO 22 | DS3231 SCL |
| VCC | 3.3V | DS3231 VCC |
| GND | GND | DS3231 GND |
| Alarm relay IN | GPIO 25 | Relay signal pin |

> Ensure the DS3231 coin cell (CR2032) is installed so time is retained across power loss.

---

## 3. First-Time Setup

1. **Power on** the ESP32.
2. On your phone or PC, open **Wi-Fi settings**.
3. Connect to the network: **`ESP32_Alarm_Clock`**
4. Password: **`12345678`**
5. A captive-portal page should pop up automatically. If not, open a browser and go to: **`http://192.168.4.1`**

---

## 4. Setting the Clock (First Step)

1. Go to the **Dashboard** page.
2. Click **"Sync DS3231 to Browser Time"**.
3. The clock will set itself to your phone/computer's current time.
4. The dashboard clock will start ticking within ~1 second.

> ⚠️ Do this first — alarms won't fire correctly until the RTC has the right time.

---

## 5. Configuring Alarms

1. Tap **Alarms** in the top navigation.
2. For each of the 8 alarm slots:
   - **Time** — pick the hour and minute.
   - **Enabled** — set to **On**.
   - **Days** — check one or more days of the week (Sun–Sat).
3. Press **Save Alarms** at the bottom.
4. Settings are saved automatically (may take up to 5 minutes to persist to flash; they work immediately).

**Alarm behavior:**
- The relay on GPIO 25 turns **ON** for **60 seconds** when an alarm triggers.
- If two alarms fire in the same minute, the 60-second window extends.

---

## 6. Display & RTC Settings

Go to the **RTC** page to customize:

| Setting | Purpose |
|---|---|
| **Aging offset** | Corrects RTC drift (−128 to 127; higher = slower). Leave at 0 unless your clock drifts. |
| **Clock format** | 24-hour or 12-hour (AM/PM). |
| **Show seconds** | Toggle seconds display on the dashboard. |
| **Timezone label** | Display only (e.g., `America/New_York`). Does **not** adjust alarm times. |
| **Sync Now** | Re-sync time from your browser. |

> The timezone is a **label only** — alarms fire at the RTC's raw time, not converted to your timezone.

---

## 7. Changing Wi-Fi Network Name / Password

Go to the **WiFi AP** page:

| Field | Range |
|---|---|
| SSID | 1–32 characters |
| Password | 8–63 characters |
| Channel | 1–13 |
| Max clients | 1–8 |
| Hidden SSID | Yes/No |

Press **Save & Restart AP**.

> ⚠️ **You will be disconnected** when you save. Reconnect using the new SSID/password. The browser redirect may not reach you — just reconnect manually.

---

## 8. Erasing Everything (Factory Reset)

**Hold the BOOT button (GPIO 0) for 5 seconds.**

- All alarms, Wi-Fi settings, and RTC preferences are erased.
- Device restarts with default settings.
- Reconnect to `ESP32_Alarm_Clock` / `12345678`.

---

## 9. Daily Usage Tips

- **Dashboard** — shows live clock, current AP info, and a sync button.
- Leave the ESP32 powered; the DS3231 keeps time even during power loss (battery).
- The AP is always on — no internet connection needed.
- Multiple phones/laptops can connect at once (up to `Max clients` setting).

---

## 10. Troubleshooting

| Problem | Fix |
|---|---|
| Clock shows `--:--:--` | RTC not detected — check SDA/SCL wiring and 3.3V power. |
| Clock is wrong | Click **Sync DS3231 to Browser Time** on Dashboard. |
| Alarm doesn't fire | Verify: alarm is **Enabled**, correct **days** checked, RTC time is correct. |
| Can't connect to AP | Hold BOOT 5s to factory reset; reconnect with default credentials. |
| Forgot AP password | Hold BOOT 5s to factory reset. |
| Web page doesn't load | Make sure you're connected to the ESP32's Wi-Fi (not your home network). |
| Dashboard shows old SSID after change | Reconnect to the new SSID, then reload the page. |
| Alarm relay never turns off | Power-cycle the device — relay should be LOW at boot. |

---

## 11. Quick Reference — Default Values

| Item | Default |
|---|---|
| AP SSID | `ESP32_Alarm_Clock` |
| AP Password | `12345678` |
| AP IP | `192.168.4.1` |
| Channel | 6 |
| Max clients | 4 |
| RTC time | 2020-01-01 00:00:00 (if battery lost) |
| Alarms | All disabled |
| Clock format | 24-hour |
| Seconds | Shown |
| Timezone label | `UTC0` |

---

## 12. Notes

- Settings are saved to flash with a **~5-minute delay** (to protect the flash memory). If power is cut within 5 minutes of a change, that change may be lost.
- Wi-Fi AP settings are saved **immediately**.
- The device runs its own Wi-Fi network — it does **not** connect to your home Wi-Fi.
- No internet or cloud is required.
