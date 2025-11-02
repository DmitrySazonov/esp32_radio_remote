# ESP32 Radio Remote

Small ESP32-based remote for controlling a phone radio/streaming app over HTTP.
It shows station and “now playing” on a 128×64 SSD1306 OLED, has an encoder for volume and play/pause, dedicated Prev/Next buttons, night dimming, and a Screen Off idle timer. A built-in web page lets you configure Wi-Fi and behavior without reflashing.

## Repository Layout

```bash
.
├─ fritzing/     # Fritzing schematic (.fzz) + exported image(s)
├─ stl/          # 3D-printable case parts
└─ esp32-radio-remote.ino  # main Arduino sketch
```

## Features (quick list)

- Encoder: rotate = volume (with optional acceleration), press:
  - single click = Play/Pause
  - rotate while pressed = Station tuning (Prev/Next)
- Buttons: dedicated Prev and Next GPIO buttons
- OLED UI: station title + now playing, volume bar, optional Wi-Fi badge (STA/AP)
- Night mode: dim to a low brightness after inactivity
- Screen Off: fully power down the OLED after a longer timeout
- Web UI (STA/AP): configure Wi-Fi SSID/Password, app host/port/token, scrolling, brightness levels, and more

- Resilience:
  - If Wi-Fi is down: device switches to AP config portal
  - If phone/app is unreachable: shows Phone offline. Retry connection... and retries with growing timeouts (up to 10s), then continues polling

## Hardware

- MCU: ESP32 DevKit (ESP-WROOM-32 or similar)
- Display: SSD1306 128×64 (I²C, address 0x3C)
- Encoder: 2-bit incremental with push button
- Buttons: two momentary buttons for Prev and Next

See fritzing/ for exact wiring (.fzz) and a PNG preview.
STL files for the enclosure live in stl/.

### Default pins (can be edited in the sketch):

- Encoder: CLK 32, DT 33, SW 25
- I²C: SDA 21, SCL 22
- Prev/Next: 26, 27 (wired to GND, use INPUT_PULLUP)

## Requirements

- Arduino IDE (2.x recommended)
- ESP32 core by Espressif (Boards Manager)
- Libraries (Library Manager):
  - Adafruit SSD1306
  - Adafruit GFX Library
  - ArduinoJson
  - (Included with ESP32 core: ESPmDNS, DNSServer, ArduinoOTA)

## Installation & Flashing (Arduino IDE)

1. Install ESP32 support

   - File → Preferences → “Additional Boards Manager URLs”: 
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   - Tools → Board → Boards Manager… → install esp32 by Espressif Systems.
     
2. Install libraries
   - Tools → Manage Libraries…
   - Search & install: Adafruit SSD1306, Adafruit GFX Library, ArduinoJson.

3. Open the sketch
    - File → Open… → select esp32-radio-remote.ino in repo root.

4. Select your board & port
   - Tools → Board: choose your ESP32 (e.g., ESP32 Dev Module or your exact variant).
   - Tools → Port: pick the COM/tty of your ESP32.

5. Compile & Upload
   - Sketch → Upload (or the right-arrow button).
   - (Optional) open Tools → Serial Monitor at 115200 baud to see boot logs.

📝 You don’t need to hardcode your Wi-Fi or phone host in the code. After the first boot, the device exposes a Config Portal in AP mode.

## First Boot & Config Portal

 - On the very first boot (or when your configured Wi-Fi is unavailable), the device enters AP mode:
   - SSID: RadioRemote-XXXXXXXX (based on MAC)
   - Password: 12345678
   - Open http://192.168.4.1/
 - After saving, it reboots and tries connecting to your Wi-Fi and your phone app.

Tip: In STA mode you can also try http://radio-remote.local/ or use the device IP (triple-click the encoder to show IP/SSID on OLED).

## Web UI — Settings Explained

Open the device IP in a browser (or http://radio-remote.local/).

### Wi-Fi & Phone App
- WiFi SSID / Password — your network credentials.
- Phone IP / Host — hostname or IP of the phone running the radio/streaming app (the device calls your app’s HTTP endpoints).
- Port — HTTP port of the app (default 8080).
- Token (optional) — if your app expects a token query parameter, it will be appended automatically to every request.

### Encoder
- Step (0.002–0.05) — volume increment per detent (float, e.g. 0.01).
- Acceleration — increases step temporarily when you spin the knob fast.
- Invert direction — flips the volume/tuning direction.

### Title Scrolling
- Mode:
  - Off — never scroll; up to 2 lines are shown if they fit.
  - Auto — if text overflows, use one-line marquee; otherwise 2 lines.
  - Always marquee — always scroll as a single line.
- Interval, ms — scrolling speed (e.g., 200 ms).

### UI
- Show network badge (STA/AP) — small status box in the top-right corner:
  - Mode: STA or AP
  - RSSI indicator (++, + , or - ) if connected

### Display
- Brightness steps (CSV) — comma-separated list of OLED brightness levels (0..255), sorted and deduplicated automatically.
Example: 15,35,70,120,180,255 

Double-click the encoder to cycle levels.

### Night mode
- Enable night mode — after inactivity, dim the screen.
- Timeout, seconds — inactivity period before dimming.
- Night brightness (0..255) — target dim value.

### Screen Off
- Enable screen off — after longer inactivity, power down the OLED.
- Timeout, seconds — inactivity period before complete off (e.g., 3600).

### Volume
- Restore volume on boot — remember last volume and apply after restart.

### Save & Reboot
- Save & Reboot — writes settings to flash and reboots.
A small auto-redirect page returns you to / after the device comes back.

## On-Device Controls (OLED & Buttons)
- Encoder rotate: volume
- Encoder single click: play/pause
- Encoder rotate while held: station tuning (Prev/Next)
- Double click: cycle brightness levels
- Triple click: show network info (IP/SSID) overlay
- Prev/Next buttons: change station immediately

### Status messages on OLED
- If Wi-Fi is unavailable (no IP): device switches to AP mode and shows config hints.
- If phone/app is unreachable (Wi-Fi OK but HTTP fails): display only