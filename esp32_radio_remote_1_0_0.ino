/*
 * ESP32 Radio Remote v1.0.0
 * STA веб: http://radio-remote.local/ или http://<IP_ESP32>/
 */

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <math.h>

// -------------------- Пины --------------------
#define PIN_ENC_CLK   32
#define PIN_ENC_DT    33
#define PIN_ENC_SW    25
#define I2C_SDA       21
#define I2C_SCL       22
// Кнопки станций:
#define PIN_BTN_PREV  26   // кнопка -> GND, INPUT_PULLUP
#define PIN_BTN_NEXT  27   // кнопка -> GND, INPUT_PULLUP

// -------------------- OLED --------------------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Символьная ширина строки при TextSize=1
const int TEXT_COLS = 18;

// -------------------- Сеть/портал --------------------
WebServer server(80);
Preferences prefs;
DNSServer dns;

String   cfg_ssid, cfg_pass, cfg_host, cfg_token;
uint16_t cfg_port = 8080;
const byte DNS_PORT = 53;

// --------- Пользовательские настройки ---------
bool     cfg_enc_invert = false;
float    cfg_enc_step   = 0.01f;
bool     cfg_enc_accel  = false;
uint16_t cfg_scroll_ms  = 200;
uint8_t  cfg_scroll_mode= 1;     // 0=Off, 1=Auto, 2=Always marquee
bool     cfg_show_badge = false;

// ---- Яркость по уровням (CSV) ----
#define BR_MAX_STEPS 12
uint8_t  cfg_steps[BR_MAX_STEPS];
uint8_t  cfg_steps_count = 0;
uint8_t  cfg_bri_idx = 0;
uint8_t  cur_brightness = 200;
const char* DEFAULT_STEPS_CSV = "15,35,70,120,180,255";

// Night mode (затемнение)
bool     cfg_night_enable  = true;
uint16_t cfg_night_timeout = 300; // сек
uint8_t  cfg_night_level   = 10;  // 0..255

// Screen Off (полное отключение дисплея)
bool     cfg_scrOff_enable  = false;
uint16_t cfg_scrOff_timeout = 3600; // сек
bool     screenOffActive    = false;

// Restore volume on boot
bool     cfg_restore_vol   = true;
float    last_saved_vol    = 1.0f;

// -------------------- Состояние UI/плеера --------------------
bool   playing = false;
bool   muted   = false;
float  volume  = 1.0f;
String stationName = "-";
String nowTitle    = "-";
int    currentIndex = -1; // из /status, если есть

// Скролл
int titleScroll = 0;
unsigned long lastScrollMs = 0;
unsigned long SCROLL_INTERVAL = 200;

// Overlay (IP, яркость и т.п.)
bool overlayActive = false;
unsigned long overlayUntilMs = 0;
String overlayL1, overlayL2, overlayL3;

// -------------------- Новая логика доступности --------------------
// Wi-Fi реконнект и переход в AP
unsigned long wifiLostSinceMs = 0;
unsigned long nextWifiReconnectMs = 0;
uint8_t wifiReconnectTry = 0;
const unsigned long WIFI_TO_AP_MS = 45000; // сколько держим попытки STA до ухода в AP

// Доступность «телефона» (HTTP-хоста)
bool     phoneOnline = false;
int      hostFailCount = 0;
unsigned long hostTimeoutMs = 2000;              // текущий таймаут HTTP
const unsigned long HOST_TIMEOUT_MIN_MS = 2000;
const unsigned long HOST_TIMEOUT_MAX_MS = 10000;
unsigned long nextHostPollMs = 0;                // когда следующий опрос /status
const unsigned long HOST_SUCCESS_POLL_MS = 2000; // частота при успехах
const float HOST_BACKOFF_FACTOR = 1.7f;          // множитель роста таймаута
const int   HOST_FAILS_TO_SHOW_STATUS = 2;       // после скольких фейлов рисуем "Phone offline"

// Спиннер для статусов
char spinnerChars[4] = {'-', '\\', '|', '/'};
uint8_t spinnerIdx = 0;

// Activity / Night / ScreenOff
unsigned long lastUserActivityMs = 0;
bool nightDimmed = false;

// Блокировка димминга после действий
unsigned long blockNightUntilMs = 0;
const unsigned long NIGHT_BLOCK_WINDOW_MS = 2000;

// -------------------- Кнопка/клики энкодера --------------------
bool btnDown = false;
unsigned long btnDownMs = 0;
const unsigned long BTN_DEBOUNCE = 25;
const unsigned long BTN_LONG_MS  = 750;

uint8_t clickCount = 0;
unsigned long lastClickMs = 0;
const unsigned long MULTI_CLICK_MS = 400;
volatile bool rotatedWhilePressed = false;

// -------------------- Энкодер: детент-декодер --------------------
static const int8_t ENC_DIR_TABLE[16] = {
  0, -1, +1, 0,
  +1, 0,  0, -1,
  -1, 0,  0, +1,
   0, +1, -1, 0
};
volatile uint8_t encState = 0;
volatile int8_t  encPulseAccum = 0;
volatile int8_t  encDetentSteps = 0;
volatile uint32_t encLastEdgeUs = 0;
const uint8_t  ENC_PULSES_PER_DETENT = 4;
const uint32_t ENC_EDGE_DEBOUNCE_US  = 250;
volatile uint32_t lastDetentUs = 0;
void IRAM_ATTR onEncEdge() {
  uint32_t now = micros();
  if (now - encLastEdgeUs < ENC_EDGE_DEBOUNCE_US) return;
  encLastEdgeUs = now;
  uint8_t a = (uint8_t)digitalRead(PIN_ENC_CLK);
  uint8_t b = (uint8_t)digitalRead(PIN_ENC_DT);
  uint8_t ab = (a << 1) | b;
  encState = ((encState << 2) | ab) & 0x0F;
  int8_t dir = ENC_DIR_TABLE[encState];
  if (dir == 0) return;
  encPulseAccum += dir;
  if (encPulseAccum >= ENC_PULSES_PER_DETENT) {
    encPulseAccum = 0; encDetentSteps += 1; lastDetentUs = now;
  } else if (encPulseAccum <= -ENC_PULSES_PER_DETENT) {
    encPulseAccum = 0; encDetentSteps -= 1; lastDetentUs = now;
  }
}

// -------------------- Громкость: пакетная отправка --------------------
float pendingVolume = 1.0f;
unsigned long lastVolSendMs = 0;
const unsigned long VOL_SEND_DEBOUNCE_MS = 60;

// -------------------- Очередь переключений станций (для вращения с зажатием) --------------------
volatile int stationMoveAccum = 0;          // >0 -> next, <0 -> prev
unsigned long lastStationSendMs = 0;
const unsigned long STATION_STEP_MS = 70;   // пауза между командами

// -------------------- Кнопки Prev/Next --------------------
const unsigned long BTN_ST_DEBOUNCE = 25;
bool btnPrevWas = true; // INPUT_PULLUP -> покой HIGH
bool btnNextWas = true;
unsigned long btnPrevChangeMs = 0;
unsigned long btnNextChangeMs = 0;

// -------------------- Прототипы --------------------
void startWiFiOrAP();
void startConfigPortal();
void startMdnsOta();
void setupHttpRoutes();
void handleRoot();
void handleSave();
void drawScreen();
void drawWifiBadge();
void drawOverlay();
void fetchStatusTry(); // ОДНА попытка опроса с текущим таймаутом/бэкоффом
void sendPlayPause();
void sendMuteToggle();
void sendNext();
void sendPrev();
bool sendVolumeOnce(float v);
void sendVolume(float v);
String buildUrl(const String& path);
void wifiToAP();
bool needsMarqueeOneLine(const String& s);
void showNetworkInfoOverlay(unsigned long ms = 3500);
void markActivity();
void ensureAwake();

// Яркость/экран
static inline uint8_t gammaMap(uint8_t v);
void applyBrightness(uint8_t v);
void cycleBrightness();
void oledDisplayOn();
void oledDisplayOff();

// Ступени яркости
bool parseStepsCSV(const String& csv, uint8_t out[], uint8_t &count);
String stepsToCSV(const uint8_t arr[], uint8_t count);
void loadBrightnessSteps();
uint8_t nearestStepIndex(uint8_t raw);

// Helpers для вывода NOW
void drawNowBlock(const String& s);

// ---- UTF-8 -> ASCII нормализация пунктуации ----
String sanitizeUtf8ToAscii(const String& in) {
  String out; out.reserve(in.length());
  for (size_t i = 0; i < in.length(); ) {
    uint8_t c = (uint8_t)in[i];
    if (c < 0x80) { out += (char)c; i++; continue; } // ASCII
    // E2 80 xx: ‘ ’ “ ” – — …
    if (i + 2 < in.length() &&
        (uint8_t)in[i]   == 0xE2 &&
        (uint8_t)in[i+1] == 0x80) {
      uint8_t t = (uint8_t)in[i+2];
      if (t == 0x98 || t == 0x99) { out += '\''; i += 3; continue; }  // ‘ ’ -> '
      if (t == 0x9C || t == 0x9D) { out += '\"'; i += 3; continue; }  // “ ” -> "
      if (t == 0x93 || t == 0x94) { out += '-';  i += 3; continue; }  // – — -> -
      if (t == 0xA6)              { out += "...";i += 3; continue; }  // … -> ...
    }
    // NBSP: C2 A0
    if (i + 1 < in.length() &&
        (uint8_t)in[i]   == 0xC2 &&
        (uint8_t)in[i+1] == 0xA0) { out += ' '; i += 2; continue; }
    // Прочие non-ASCII — пропускаем
    i += ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : 1;
  }
  return out;
}

// -------------------- Setup --------------------
void setup() {
  pinMode(PIN_ENC_CLK, INPUT_PULLUP);
  pinMode(PIN_ENC_DT,  INPUT_PULLUP);
  pinMode(PIN_ENC_SW,  INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), onEncEdge, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_DT),  onEncEdge, CHANGE);

  pinMode(PIN_BTN_PREV, INPUT_PULLUP);
  pinMode(PIN_BTN_NEXT, INPUT_PULLUP);

  Wire.begin(I2C_SDA, I2C_SCL);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  display.println("Radio Remote v1.0.0");
  display.display();

  prefs.begin("cfg", false);
  cfg_ssid  = prefs.getString("ssid", "");
  cfg_pass  = prefs.getString("pass", "");
  cfg_host  = prefs.getString("host", "");
  cfg_port  = prefs.getUShort("port", 8080);
  cfg_token = prefs.getString("token", "");

  cfg_enc_invert   = prefs.getBool ("enc_inv", false);
  cfg_enc_step     = prefs.getFloat("enc_step", 0.01f);
  cfg_enc_accel    = prefs.getBool ("enc_acc", false);
  cfg_scroll_ms    = prefs.getUShort("scr_ms", 200);
  cfg_scroll_mode  = prefs.getUChar ("scr_mode", 1);
  cfg_show_badge   = prefs.getBool ("badge", false);

  // Screen Off
  cfg_scrOff_enable  = prefs.getBool ("scrOff_en", false);
  cfg_scrOff_timeout = prefs.getUShort("scrOff_to", 3600);

  // Яркость
  loadBrightnessSteps();
  cfg_bri_idx = prefs.getUChar("bri_idx", 0);
  if (cfg_bri_idx >= cfg_steps_count) cfg_bri_idx = 0;

  uint8_t legacy_raw = prefs.getUChar("bright_raw", 255);
  if (legacy_raw != 255) {
    cfg_bri_idx = nearestStepIndex(legacy_raw);
    prefs.remove("bright_raw");
    prefs.putUChar("bri_idx", cfg_bri_idx);
  }
  cur_brightness = cfg_steps[cfg_bri_idx];
  applyBrightness(cur_brightness);
  oledDisplayOn(); // на всякий

  // Night mode
  cfg_night_enable  = prefs.getBool ("night_en", true);
  cfg_night_timeout = prefs.getUShort("night_to", 300);
  cfg_night_level   = prefs.getUChar ("night_lvl", 10);
  if (cfg_night_level > 255) cfg_night_level = 255;

  // Громкость
  cfg_restore_vol   = prefs.getBool ("restore_vol", true);
  last_saved_vol    = prefs.getFloat("last_vol", 1.0f);
  if (last_saved_vol < 0.0f || last_saved_vol > 1.0f) last_saved_vol = 1.0f;

  // Валидация
  if (cfg_enc_step   < 0.002f) cfg_enc_step   = 0.002f;
  if (cfg_enc_step   > 0.05f)  cfg_enc_step   = 0.05f;
  if (cfg_scroll_ms  < 50)     cfg_scroll_ms  = 50;
  if (cfg_scroll_ms  > 2000)   cfg_scroll_ms  = 2000;
  if (cfg_scroll_mode > 2)     cfg_scroll_mode= 1;
  if (cfg_scrOff_timeout < 30) cfg_scrOff_timeout = 30;
  if (cfg_scrOff_timeout > 86400) cfg_scrOff_timeout = 86400;

  SCROLL_INTERVAL = cfg_scroll_ms;

  startWiFiOrAP();
  drawScreen();

  lastUserActivityMs = millis();
  pendingVolume = volume;

  // Инициализация статусов
  phoneOnline = false;
  hostTimeoutMs = HOST_TIMEOUT_MIN_MS;
  nextHostPollMs = millis(); // сразу пробовать
}

// -------------------- Loop --------------------
void loop() {
  if (WiFi.getMode() == WIFI_AP) dns.processNextRequest();
  server.handleClient();
  ArduinoOTA.handle();

  unsigned long nowMs = millis();

  // ---------------- Wi-Fi состояние ----------------
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiLostSinceMs == 0) wifiLostSinceMs = nowMs;

    // экспоненциальный бэкофф реконнекта
    if (nowMs >= nextWifiReconnectMs) {
      WiFi.reconnect();
      wifiReconnectTry = min<uint8_t>(wifiReconnectTry + 1, 10);
      uint32_t backoff = 300 + (uint32_t)pow(1.8, wifiReconnectTry) * 100;
      nextWifiReconnectMs = nowMs + backoff;
    }

    // Долго нет Wi-Fi -> AP
    if ((nowMs - wifiLostSinceMs) > WIFI_TO_AP_MS) {
      wifiToAP();
      // В AP мы не делаем никаких HTTP-попыток
    }

  } else {
    // Был коннект — сбросить счётчики
    wifiLostSinceMs = 0;
    wifiReconnectTry = 0;

    // ---------------- HTTP-хост («телефон») ----------------
    if (nowMs >= nextHostPollMs && WiFi.getMode() == WIFI_STA) {
      fetchStatusTry(); // корректирует nextHostPollMs/hostTimeoutMs/флаги
      if (!overlayActive && !screenOffActive) drawScreen();
    }
  }

  // Прокрутка NOW + спиннер
  if (nowMs - lastScrollMs >= SCROLL_INTERVAL) {
    lastScrollMs = nowMs;
    bool overflow = needsMarqueeOneLine(nowTitle);
    bool scrollActive = (cfg_scroll_mode == 2) || (cfg_scroll_mode == 1 && overflow);
    if (scrollActive && nowTitle.length() > 0) {
      int ringLen = nowTitle.length() + 3;
      if (ringLen <= 0) ringLen = 1;
      titleScroll = (titleScroll + 1) % ringLen;
      if (!overlayActive && !screenOffActive) drawScreen();
    } else titleScroll = 0;
    spinnerIdx = (spinnerIdx + 1) & 3;
  }

  // Overlay TTL
  if (overlayActive && nowMs >= overlayUntilMs) { overlayActive = false; if (!screenOffActive) drawScreen(); }

  // Night mode (затемнение, но не Screen Off)
  if (cfg_night_enable) {
    if (!nightDimmed) {
      bool gestureActive = btnDown || rotatedWhilePressed;
      if (!gestureActive && nowMs >= blockNightUntilMs) {
        if ((nowMs - lastUserActivityMs) > (uint32_t)cfg_night_timeout * 1000UL) {
          applyBrightness(cfg_night_level);
          nightDimmed = true;
        }
      }
    }
  }

  // Screen Off (полное отключение дисплея)
  if (cfg_scrOff_enable) {
    if (!screenOffActive) {
      bool gestureActive = btnDown || rotatedWhilePressed;
      if (!gestureActive && nowMs >= blockNightUntilMs) {
        if ((nowMs - lastUserActivityMs) > (uint32_t)cfg_scrOff_timeout * 1000UL) {
          oledDisplayOff();
          screenOffActive = true;
        }
      }
    }
  }

  // Кнопки Prev/Next (дребезг)
  bool prevNow = (digitalRead(PIN_BTN_PREV) == LOW);
  bool nextNow = (digitalRead(PIN_BTN_NEXT) == LOW);

  if (prevNow != btnPrevWas && (nowMs - btnPrevChangeMs) > BTN_ST_DEBOUNCE) {
    btnPrevWas = prevNow; btnPrevChangeMs = nowMs;
    if (prevNow) { sendPrev(); if (!overlayActive && !screenOffActive) drawScreen(); markActivity(); ensureAwake(); }
  }
  if (nextNow != btnNextWas && (nowMs - btnNextChangeMs) > BTN_ST_DEBOUNCE) {
    btnNextWas = nextNow; btnNextChangeMs = nowMs;
    if (nextNow) { sendNext(); if (!overlayActive && !screenOffActive) drawScreen(); markActivity(); ensureAwake(); }
  }

  // Мультиклики энкодера
  bool sw = (digitalRead(PIN_ENC_SW) == LOW);
  static bool btnWas = false;
  static unsigned long btnChangeMs = 0;
  if (sw != btnWas && (nowMs - btnChangeMs) > BTN_DEBOUNCE) {
    btnWas = sw; btnChangeMs = nowMs;
    if (sw) { btnDown = true; btnDownMs = nowMs; rotatedWhilePressed = false; }
    else {
      bool wasLong = (nowMs - btnDownMs) >= BTN_LONG_MS; btnDown = false;
      if (rotatedWhilePressed) { rotatedWhilePressed = false; markActivity(); ensureAwake(); }
      else {
        if (wasLong) { clickCount = 0; sendMuteToggle(); markActivity(); ensureAwake(); }
        else { clickCount++; lastClickMs = nowMs; markActivity(); ensureAwake(); }
      }
    }
  }
  if (clickCount > 0 && (nowMs - lastClickMs) > MULTI_CLICK_MS) {
    uint8_t n = clickCount; clickCount = 0;
    if (n >= 3) { showNetworkInfoOverlay(3500); markActivity(); ensureAwake(); }
    else if (n == 2) { markActivity(); ensureAwake(); cycleBrightness(); }
    else if (n == 1) { sendPlayPause(); markActivity(); ensureAwake(); }
  }

  // Энкодер детенты
  int8_t detents;
  noInterrupts(); detents = encDetentSteps; encDetentSteps = 0; interrupts();
  if (detents != 0) {
    int dir = cfg_enc_invert ? -1 : 1;
    int move = detents * dir;

    if (digitalRead(PIN_ENC_SW) == LOW || btnDown) {
      rotatedWhilePressed = true;
      stationMoveAccum += move;
      markActivity(); ensureAwake();
    } else {
      if (cfg_enc_accel) {
        uint32_t usSince = micros() - lastDetentUs;
        int accel = (usSince < 1200) ? 3 : (usSince < 2500) ? 2 : 1;
        move *= accel;
      }
      pendingVolume = constrain(pendingVolume + cfg_enc_step * move, 0.0f, 1.0f);
      markActivity(); ensureAwake();
    }
  }

  // Очередь станций (для вращения)
  if (stationMoveAccum != 0 && (nowMs - lastStationSendMs) >= STATION_STEP_MS) {
    if (stationMoveAccum > 0) { sendNext(); stationMoveAccum -= 1; }
    else                      { sendPrev(); stationMoveAccum += 1; }
    lastStationSendMs = nowMs;
    if (!overlayActive && !screenOffActive) drawScreen();
  }

  // Громкость — отправка пакетно
  if (fabs(pendingVolume - volume) >= (cfg_enc_step * 0.5f)) {
    if ((nowMs - lastVolSendMs) >= VOL_SEND_DEBOUNCE_MS) {
      if (sendVolumeOnce(pendingVolume)) {
        volume = pendingVolume;
        prefs.putFloat("last_vol", volume);
      }
      lastVolSendMs = nowMs;
      if (!overlayActive && !screenOffActive) drawScreen();
    }
  }

  delay(3);
}

// -------------------- Wi-Fi & портал --------------------
void startWiFiOrAP() {
  if (cfg_ssid.length() > 0 && cfg_host.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg_ssid.c_str(), cfg_pass.c_str());

    display.clearDisplay();
    display.setCursor(0,0);
    display.println("WiFi connect...");
    display.println(cfg_ssid);
    display.display();

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
      delay(250); display.print("."); display.display();
    }
    if (WiFi.status() == WL_CONNECTED) {
      display.println(); display.println("OK: " + WiFi.localIP().toString()); display.display();
      delay(500);
      startMdnsOta();
      setupHttpRoutes(); server.begin();

      if (cfg_restore_vol) {
        float v = constrain(last_saved_vol, 0.0f, 1.0f);
        if (sendVolumeOnce(v)) { volume = v; pendingVolume = v; }
      }
      nextHostPollMs = millis(); // сразу пробуем хост
      return;
    }
  }
  startConfigPortal();
}

void startConfigPortal() {
  WiFi.mode(WIFI_AP);
  String apName = "RadioRemote-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  apName.toUpperCase();
  WiFi.softAP(apName.c_str(), "12345678");
  IPAddress ip = WiFi.softAPIP();
  dns.start(DNS_PORT, "*", ip);

  setupHttpRoutes(); server.begin();

  display.clearDisplay();
  display.setCursor(0,0);
  display.println("CONFIG MODE");
  display.println("AP: " + apName);
  display.println("PW: 12345678");
  display.println("Open: 192.168.4.1");
  display.display();
}

void startMdnsOta() {
  if (MDNS.begin("radio-remote")) MDNS.addService("http","tcp",80);
  ArduinoOTA.setHostname("radio-remote");
  ArduinoOTA.begin();
}

// -------------------- HTTP-маршруты --------------------
void setupHttpRoutes() {
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
}

// -------------------- Веб-страницы --------------------
void handleRoot() {
  String stepsCsv = stepsToCSV(cfg_steps, cfg_steps_count);

  String html = R"(
<!doctype html><html><head><meta charset="utf-8"><meta name=viewport content="width=device-width,initial-scale=1">
<title>Radio Remote Setup</title>
<style>
body{font-family:sans-serif;max-width:680px;margin:20px auto}
label{display:block;margin:10px 0 4px}
input,select{width:100%;padding:8px}
.small{font-size:12px;color:#666}
.row{display:flex;gap:10px} .row>div{flex:1}
button{margin-top:16px;padding:10px 14px}
code{background:#f3f3f3;padding:2px 6px;border-radius:4px}
</style>
</head><body>
<h2>ESP32 Radio Remote — Setup</h2>
<form method="POST" action="/save">
)";
  html += "<label>WiFi SSID</label><input name=\"ssid\" required value=\"" + cfg_ssid + "\">";
  html += "<label>WiFi Password</label><input name=\"pass\" type=\"password\" value=\"" + cfg_pass + "\">";
  html += "<label>Phone IP / Host</label><input name=\"host\" required value=\"" + cfg_host + "\">";
  html += "<div class=\"row\"><div><label>Port</label><input name=\"port\" value=\"" + String(cfg_port) + "\"></div>";
  html += "<div><label>Token (optional)</label><input name=\"token\" value=\"" + cfg_token + "\"></div></div>";

  // Encoder
  html += "<h3>Encoder</h3><div class=\"row\">";
  html += "<div><label>Step (0.002–0.05)</label><input name=\"enc_step\" value=\"" + String(cfg_enc_step,3) + "\"></div>";
  html += "<div><label>Acceleration</label><select name=\"enc_accel\">";
  html += String("<option value=\"0\"") + (cfg_enc_accel ? "" : " selected") + ">Off</option>";
  html += String("<option value=\"1\"") + (cfg_enc_accel ? " selected" : "") + ">On</option>";
  html += "</select></div></div>";
  html += String("<label><input type=\"checkbox\" name=\"enc_inv\" value=\"1\"") + (cfg_enc_invert ? " checked" : "") + "> Invert direction</label>";

  // Scrolling
  html += "<h3>Title scrolling</h3><div class=\"row\"><div><label>Mode</label><select name=\"scr_mode\">";
  html += String("<option value=\"0\"") + (cfg_scroll_mode==0 ? " selected" : "") + ">Off</option>";
  html += String("<option value=\"1\"") + (cfg_scroll_mode==1 ? " selected" : "") + ">Auto (2 lines if fits; 1-line marquee if not)</option>";
  html += String("<option value=\"2\"") + (cfg_scroll_mode==2 ? " selected" : "") + ">Always marquee</option>";
  html += "</select></div>";
  html += "<div><label>Interval, ms</label><input name=\"scr_ms\" value=\"" + String(cfg_scroll_ms) + "\"></div></div>";

  // UI
  html += String("<h3>UI</h3><label><input type=\"checkbox\" name=\"badge\" value=\"1\"") + (cfg_show_badge ? " checked" : "") + "> Show network badge (STA/AP)</label>";

  // Brightness steps
  html += "<h3>Display</h3>";
  html += "<label>Brightness steps (CSV, 1–12 numbers 0..255)</label>";
  html += String("<input name=\"bright_steps\" value=\"") + stepsCsv + "\">";
  html += "<div class=\"small\">Example: <code>15,35,70,120,180,255</code>. Double-click cycles levels.</div>";
  html += String("<div class=\"small\">Current index: ") + String((int)cfg_bri_idx) + ", value: " + String((int)cfg_steps[cfg_bri_idx]) + "</div>";

  // Night mode
  html += "<h3>Night mode</h3>";
  html += String("<label><input type=\"checkbox\" name=\"night_en\" value=\"1\"") + (cfg_night_enable ? " checked" : "") + "> Enable night mode</label>";
  html += "<div class=\"row\"><div><label>Timeout, seconds</label><input name=\"night_to\" value=\"" + String(cfg_night_timeout) + "\"></div>";
  html += "<div><label>Night brightness (0..255)</label><input name=\"night_lvl\" value=\"" + String((int)cfg_night_level) + "\"></div></div>";

  // Screen Off
  html += "<h3>Screen Off</h3>";
  html += String("<label><input type=\"checkbox\" name=\"scrOff_en\" value=\"1\"") + (cfg_scrOff_enable ? " checked" : "") + "> Enable screen off</label>";
  html += "<div><label>Timeout, seconds</label><input name=\"scrOff_to\" value=\"" + String(cfg_scrOff_timeout) + "\"></div>";

  // Restore volume
  html += "<h3>Volume</h3>";
  html += String("<label><input type=\"checkbox\" name=\"restore_vol\" value=\"1\"") + (cfg_restore_vol ? " checked" : "") + "> Restore volume on boot</label>";

  html += R"(
<button type="submit">Save & Reboot</button>
<p class="small">STA-mode: <b>http://radio-remote.local/</b> or by IP (3× click shows IP).</p>
</form></body></html>
)";
  server.send(200, "text/html", html);
}

void handleSave() {
  String ssid  = server.arg("ssid");
  String pass  = server.arg("pass");
  String host  = server.arg("host");
  String portS = server.arg("port");
  String token = server.arg("token");

  bool enc_inv    = server.hasArg("enc_inv");
  float enc_step  = server.hasArg("enc_step") ? server.arg("enc_step").toFloat() : 0.01f;
  bool enc_accel  = (server.arg("enc_accel") == "1");
  uint16_t scr_ms = server.hasArg("scr_ms") ? (uint16_t)server.arg("scr_ms").toInt() : 200;
  uint8_t scr_mode= server.hasArg("scr_mode") ? (uint8_t)server.arg("scr_mode").toInt() : 1;
  bool badge      = server.hasArg("badge");

  // Steps CSV
  String stepsCsvIn = server.hasArg("bright_steps") ? server.arg("bright_steps") : DEFAULT_STEPS_CSV;
  uint8_t tmpSteps[BR_MAX_STEPS]; uint8_t tmpN = 0;
  bool stepsOk = parseStepsCSV(stepsCsvIn, tmpSteps, tmpN);
  if (!stepsOk || tmpN == 0) { parseStepsCSV(DEFAULT_STEPS_CSV, tmpSteps, tmpN); stepsCsvIn = DEFAULT_STEPS_CSV; }

  // Night mode
  bool night_en   = server.hasArg("night_en");
  uint16_t night_to = server.hasArg("night_to") ? (uint16_t)server.arg("night_to").toInt() : 300;
  uint16_t night_lvl = server.hasArg("night_lvl") ? (uint16_t)server.arg("night_lvl").toInt() : 10;

  // Screen Off
  bool scrOff_en    = server.hasArg("scrOff_en");
  uint16_t scrOff_to= server.hasArg("scrOff_to") ? (uint16_t)server.arg("scrOff_to").toInt() : 3600;

  // Restore volume
  bool restore_vol = server.hasArg("restore_vol");

  // Валидация
  if (ssid.isEmpty() || host.isEmpty()) { server.send(400,"text/plain","ssid and host required"); return; }
  uint16_t port = portS.length() ? portS.toInt() : 8080;

  if (enc_step < 0.002f) enc_step = 0.002f;
  if (enc_step > 0.05f)  enc_step = 0.05f;
  if (scr_ms   < 50)     scr_ms   = 50;
  if (scr_ms   > 2000)   scr_ms   = 2000;
  if (scr_mode > 2)      scr_mode = 1;
  if (night_to < 10)     night_to = 10;
  if (night_to > 36000)  night_to = 36000;
  if (night_lvl > 255)   night_lvl= 255;
  if (scrOff_to < 30)    scrOff_to = 30;
  if (scrOff_to > 86400) scrOff_to = 86400;

  // Сохраняем
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putString("host", host);
  prefs.putUShort("port", port);
  prefs.putString("token", token);

  prefs.putBool ("enc_inv", enc_inv);
  prefs.putFloat("enc_step", enc_step);
  prefs.putBool ("enc_acc", enc_accel);
  prefs.putUShort("scr_ms",  scr_ms);
  prefs.putUChar("scr_mode", scr_mode);
  prefs.putBool ("badge",    badge);

  // Яркость
  prefs.putString("bright_steps", stepsCsvIn);
  parseStepsCSV(stepsCsvIn, cfg_steps, cfg_steps_count);
  cfg_bri_idx = nearestStepIndex(cur_brightness);
  if (cfg_bri_idx >= cfg_steps_count) cfg_bri_idx = 0;
  prefs.putUChar("bri_idx", cfg_bri_idx);

  // Night
  prefs.putBool ("night_en",  night_en);
  prefs.putUShort("night_to", night_to);
  prefs.putUChar ("night_lvl",(uint8_t)night_lvl);

  // Screen Off
  prefs.putBool ("scrOff_en",  scrOff_en);
  prefs.putUShort("scrOff_to", scrOff_to);

  // Volume restore
  prefs.putBool ("restore_vol", restore_vol);

  // HTML со смарт-редиректом и ребутом
  String html =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta http-equiv='refresh' content='10;url=/'/>"
    "<title>Saved</title></head><body>"
    "<h3>Saved. Rebooting...</h3>"
    "<p>The device will reboot now. This page will try to reopen <a href=\"/\">/</a> in 10 seconds.</p>"
    "<script>setTimeout(function(){location.replace('/')}, 12000);</script>"
    "<p>If IP changes (STA mode), open <b>http://radio-remote.local/</b> or check IP (triple click).</p>"
    "</body></html>";

  server.send(200, "text/html", html);
  delay(1500);
  ESP.restart();
}

// -------------------- Работа с телефоном --------------------
String buildUrl(const String& path) {
  String url = "http://" + cfg_host + ":" + String(cfg_port) + path;
  if (cfg_token.length()) { url += (url.indexOf('?') >= 0 ? "&" : "?"); url += "token=" + cfg_token; }
  return url;
}

bool httpGetSimple(const String& url) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http; http.begin(url); http.setTimeout(hostTimeoutMs);
  int code = http.GET();
  http.end();
  return code == 200;
}

// Одна попытка опроса статуса «телефона» с учётом бэкоффа/таймаута
void fetchStatusTry() {
  unsigned long nowMs = millis();

  if (cfg_host.isEmpty() || WiFi.status() != WL_CONNECTED) {
    phoneOnline = false;
    nextHostPollMs = nowMs + 1000;
    return;
  }

  HTTPClient http;
  String url = buildUrl("/status");
  http.begin(url);
  http.setTimeout(hostTimeoutMs);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      playing = doc["playing"] | false;
      if (doc.containsKey("currentIndex")) currentIndex = doc["currentIndex"].as<int>();

      const char* st = doc["station"].is<const char*>() ? doc["station"].as<const char*>() : "-";
      const char* tt = doc["title"].is<const char*>()   ? doc["title"].as<const char*>()   : "-";
      stationName = sanitizeUtf8ToAscii(String(st));
      nowTitle    = sanitizeUtf8ToAscii(String(tt));

      if (doc.containsKey("volume")) {
        volume = constrain(doc["volume"].as<float>(), 0.0f, 1.0f);
        pendingVolume = volume;
      }
      if (doc.containsKey("muted"))  muted  = doc["muted"].as<bool>();

      phoneOnline = true;
      hostFailCount = 0;
      hostTimeoutMs = HOST_TIMEOUT_MIN_MS;
      nextHostPollMs = nowMs + HOST_SUCCESS_POLL_MS;
    } else {
      phoneOnline = false;
      hostFailCount++;
      hostTimeoutMs = min((unsigned long)floor(hostTimeoutMs * HOST_BACKOFF_FACTOR), HOST_TIMEOUT_MAX_MS);
      nextHostPollMs = nowMs + hostTimeoutMs;
    }
  } else {
    phoneOnline = false;
    hostFailCount++;
    hostTimeoutMs = min((unsigned long)floor(hostTimeoutMs * HOST_BACKOFF_FACTOR), HOST_TIMEOUT_MAX_MS);
    nextHostPollMs = nowMs + hostTimeoutMs;
  }

  http.end();
}

void sendPlayPause() {
  if (!cfg_host.length()) return;
  String url = buildUrl(playing ? "/pause" : "/play");
  if (httpGetSimple(url)) { playing = !playing; if (!overlayActive && !screenOffActive) drawScreen(); }
}
void sendMuteToggle(){
  if (!cfg_host.length()) return;
  String url = buildUrl(muted ? "/unmute" : "/mute");
  if (httpGetSimple(url)) { muted = !muted;   if (!overlayActive && !screenOffActive) drawScreen(); }
}
void sendNext()   { if (!cfg_host.length()) return; (void)httpGetSimple(buildUrl("/next")); }
void sendPrev()   { if (!cfg_host.length()) return; (void)httpGetSimple(buildUrl("/prev")); }

bool sendVolumeOnce(float v) {
  if (!cfg_host.length() || WiFi.status()!=WL_CONNECTED) return false;
  char buf[24]; dtostrf(v, 1, 2, buf);
  String url = buildUrl(String("/volume?value=") + buf);
  return httpGetSimple(url);
}
void sendVolume(float v) { (void)sendVolumeOnce(v); }

// -------------------- Overlay / экран --------------------
void showNetworkInfoOverlay(unsigned long ms) {
  overlayL1 = "Network info"; overlayL2 = "-"; overlayL3 = "-";
  if (WiFi.getMode() == WIFI_STA && WiFi.status() == WL_CONNECTED) {
    overlayL2 = WiFi.localIP().toString();
    overlayL3 = "SSID: " + WiFi.SSID();
  } else if (WiFi.getMode() == WIFI_AP) {
    overlayL2 = WiFi.softAPIP().toString(); overlayL3 = "AP mode";
  } else { overlayL2 = "No STA IP"; overlayL3 = "Not connected"; }
  overlayActive = true; overlayUntilMs = millis() + ms;
  ensureAwake();
  drawOverlay();
}

void drawOverlay() {
  display.clearDisplay();
  display.setTextSize(1); display.setCursor(0,0);
  display.println(overlayL1); display.println(overlayL2); display.println(overlayL3);
  display.setCursor(0, SCREEN_HEIGHT - 10); display.println("Close soon...");
  display.display();
}

bool needsMarqueeOneLine(const String& s) {
  return (int)s.length() > (2 * TEXT_COLS);
}

// ---- КЛЮЧЕВАЯ правка: чистые экраны статуса Wi-Fi/Phone ----
void drawScreen() {
  if (overlayActive) { drawOverlay(); return; }
  if (screenOffActive) return; // экран выключен — не перерисовываем

  // 1) Если нет Wi-Fi (STA), рисуем ТОЛЬКО статус Wi-Fi
  if (WiFi.getMode() == WIFI_STA && WiFi.status() != WL_CONNECTED) {
    display.clearDisplay();
    display.setTextSize(1); display.setCursor(0,0);
    display.println("WiFi offline");
    display.print("Reconnecting "); display.println(spinnerChars[spinnerIdx]);
    if (wifiLostSinceMs > 0) {
      unsigned long msLeft = (wifiLostSinceMs + WIFI_TO_AP_MS > millis())
        ? (wifiLostSinceMs + WIFI_TO_AP_MS - millis()) : 0;
      display.print("AP in ~"); display.print((int)(msLeft/1000)); display.println("s");
    }
    if (cfg_show_badge) drawWifiBadge();
    display.display();
    return;
  }

  // 2) Если Wi-Fi есть, но телефон офлайн — ТОЛЬКО простое сообщение
  if (WiFi.status() == WL_CONNECTED && !phoneOnline && hostFailCount >= HOST_FAILS_TO_SHOW_STATUS) {
    display.clearDisplay();
    display.setTextSize(1); display.setCursor(0,0);
    display.println("Phone offline.");
    display.println("Retry connection...");
    // Можно добавить "spinner" для ощущения жизни:
    display.setCursor(0, 24);
    display.print("Please wait "); display.println(spinnerChars[spinnerIdx]);
    if (cfg_show_badge) drawWifiBadge();
    display.display();
    return;
  }

  // 3) Обычный экран, когда всё ок
  display.clearDisplay();
  display.setTextSize(1); display.setCursor(0,0);

  // Station — ВСЕГДА только имя станции
  display.println("Station:");
  display.println(stationName);

  // Now
  display.println("Now:");
  drawNowBlock(nowTitle);

  // шкала громкости
  int barY = SCREEN_HEIGHT - 14;
  display.drawRect(0, barY, SCREEN_WIDTH - 20, 12, SSD1306_WHITE);
  int barW = (int)(volume * (SCREEN_WIDTH - 24));
  display.fillRect(2, barY + 2, max(0, barW), 8, SSD1306_WHITE);
  display.setCursor(SCREEN_WIDTH - 18, barY + 2);
  int vol100 = (int)roundf(volume * 100.0f); display.printf("%3d", constrain(vol100, 0, 100));

  // иконка состояния
  if (muted) {
    display.drawLine(SCREEN_WIDTH - 10, 16, SCREEN_WIDTH - 2, 24, SSD1306_WHITE);
    display.drawLine(SCREEN_WIDTH - 10, 24, SCREEN_WIDTH - 2, 16, SSD1306_WHITE);
  } else if (playing) {
    display.fillTriangle(SCREEN_WIDTH - 10, 16, SCREEN_WIDTH - 10, 26, SCREEN_WIDTH - 2, 21, SSD1306_WHITE);
  } else {
    display.fillRect(SCREEN_WIDTH - 12, 16, 3, 10, SSD1306_WHITE);
    display.fillRect(SCREEN_WIDTH - 6,  16, 3, 10, SSD1306_WHITE);
  }

  if (cfg_show_badge) drawWifiBadge();
  display.display();
}

void drawNowBlock(const String& s) {
  bool overflow = needsMarqueeOneLine(s);
  bool scrollActive = (cfg_scroll_mode == 2) || (cfg_scroll_mode == 1 && overflow);

  if (scrollActive && s.length() > 0) {
    String ring = s + "   ";
    int L = ring.length();
    if (L <= 0) { display.println(""); return; }
    int start = titleScroll % L;

    String window;
    if (start + TEXT_COLS <= L) window = ring.substring(start, start + TEXT_COLS);
    else {
      int part1 = L - start;
      window = ring.substring(start) + ring.substring(0, TEXT_COLS - part1);
    }
    display.println(window);
  } else {
    String s1 = s;
    if ((int)s1.length() > 2 * TEXT_COLS) s1 = s1.substring(0, 2 * TEXT_COLS);
    String l1 = s1.substring(0, min((int)s1.length(), TEXT_COLS));
    String l2;
    if ((int)s1.length() > TEXT_COLS) l2 = s1.substring(TEXT_COLS);
    display.println(l1);
    display.println(l2);
  }
}

void drawWifiBadge() {
  int16_t x = SCREEN_WIDTH - 36, y = 0;
  display.drawRect(x, y, 36, 12, SSD1306_WHITE);
  String mode = (WiFi.getMode() == WIFI_AP) ? "AP" : "STA";
  display.setCursor(x+2, y+2); display.setTextSize(1); display.print(mode);
  display.setCursor(x+18, y+2);
  if (WiFi.status() == WL_CONNECTED) {
    int rssi = WiFi.RSSI(); display.print((rssi > -60) ? "++" : (rssi > -75) ? "+ " : "- ");
  } else display.print("..");
}

// -------------------- Яркость/экран --------------------
static inline uint8_t gammaMap(uint8_t v) {
  float x = v / 255.0f;
  float g = 1.8f;
  return (uint8_t)roundf(powf(x, g) * 255.0f);
}
void applyBrightness(uint8_t v) {
  if (v > 255) v = 255;
  if (v == cur_brightness) return;

  uint8_t c = gammaMap(v);
  display.ssd1306_command(SSD1306_SETCONTRAST); display.ssd1306_command(c);
  uint8_t precharge = map(c, 0, 255, 0x10, 0xF1);
  display.ssd1306_command(SSD1306_SETPRECHARGE); display.ssd1306_command(precharge);
  uint8_t vcomh = (c < 20) ? 0x20 : (c < 80 ? 0x30 : 0x40);
  display.ssd1306_command(SSD1306_SETVCOMDETECT); display.ssd1306_command(vcomh);
  display.dim(c <= 6);
  cur_brightness = v;
}
void oledDisplayOn()  { display.ssd1306_command(SSD1306_DISPLAYON);  } // 0xAF
void oledDisplayOff() { display.ssd1306_command(SSD1306_DISPLAYOFF); } // 0xAE

// -------------------- BR steps helpers --------------------
bool parseStepsCSV(const String& csv, uint8_t out[], uint8_t &count) {
  uint8_t temp[BR_MAX_STEPS]; uint8_t n=0;
  String token;
  for (size_t i=0; i<=csv.length(); ++i) {
    char ch = (i<csv.length()) ? csv[i] : ','; // push last
    if (ch==',' || ch==';' || ch==' ' || ch=='\t' || ch=='\n' || ch=='\r') {
      if (token.length()) {
        long v = token.toInt();
        if (v < 0) v = 0; if (v > 255) v = 255;
        if (n < BR_MAX_STEPS) temp[n++] = (uint8_t)v;
        token = "";
      }
      if (ch==',' || ch==';') continue;
    } else token += ch;
  }
  if (n==0) { count = 0; return false; }

  // sort asc
  for (uint8_t i=1; i<n; i++) {
    uint8_t key = temp[i]; int j = i-1;
    while (j>=0 && temp[j] > key) { temp[j+1]=temp[j]; j--; if (j<0) break; }
    temp[j+1] = key;
  }
  // dedup
  uint8_t m=0, last=255; bool first=true;
  for (uint8_t i=0;i<n;i++) {
    uint8_t v = temp[i];
    if (first || v != last) {
      if (m < BR_MAX_STEPS) out[m++] = v;
      last = v; first=false;
    }
  }
  count = m;
  return m>0;
}

String stepsToCSV(const uint8_t arr[], uint8_t count) {
  if (count==0) return String(DEFAULT_STEPS_CSV);
  String s;
  for (uint8_t i=0;i<count;i++) {
    if (i) s += ",";
    s += String(arr[i]);
  }
  return s;
}

void loadBrightnessSteps() {
  String csv = prefs.getString("bright_steps", DEFAULT_STEPS_CSV);
  if (!parseStepsCSV(csv, cfg_steps, cfg_steps_count) || cfg_steps_count==0) {
    parseStepsCSV(DEFAULT_STEPS_CSV, cfg_steps, cfg_steps_count);
  }
  if (cfg_steps_count == 0) { cfg_steps[0] = 180; cfg_steps_count = 1; }
}

uint8_t nearestStepIndex(uint8_t raw) {
  uint16_t best = 1000; uint8_t idx = 0;
  for (uint8_t i=0;i<cfg_steps_count;i++) {
    uint16_t d = abs((int)cfg_steps[i] - (int)raw);
    if (d < best) { best = d; idx = i; }
  }
  return idx;
}

// 2× клик: цикл яркости
void cycleBrightness() {
  if (cfg_steps_count == 0) { loadBrightnessSteps(); }
  nightDimmed = false;
  lastUserActivityMs = millis();
  blockNightUntilMs = millis() + NIGHT_BLOCK_WINDOW_MS;

  cfg_bri_idx = (cfg_bri_idx + 1) % cfg_steps_count;
  uint8_t v = cfg_steps[cfg_bri_idx];
  prefs.putUChar("bri_idx", cfg_bri_idx);

  // Если экран был полностью Off — включаем
  if (screenOffActive) {
    oledDisplayOn();
    screenOffActive = false;
  }
  applyBrightness(v);

  int pct = (int)roundf((v / 255.0f) * 100.0f);
  overlayL1 = "Brightness"; overlayL2 = String(pct) + "%"; overlayL3 = "-";
  overlayActive = true; overlayUntilMs = millis() + 1200; drawOverlay();
}

// -------------------- Activity/Night helpers --------------------
void markActivity() {
  lastUserActivityMs = millis();

  // Экран мог быть полностью выключен — включим
  if (screenOffActive) {
    oledDisplayOn();
    screenOffActive = false;
    applyBrightness(cfg_steps[cfg_bri_idx]); // вернуть яркость
  }

  if (nightDimmed) { nightDimmed = false; applyBrightness(cfg_steps[cfg_bri_idx]); }
  blockNightUntilMs = millis() + NIGHT_BLOCK_WINDOW_MS;
}
void ensureAwake() {
  if (screenOffActive) {
    oledDisplayOn();
    screenOffActive = false;
    applyBrightness(cfg_steps[cfg_bri_idx]);
  }
  if (nightDimmed) {
    nightDimmed = false;
    applyBrightness(cfg_steps[cfg_bri_idx]);
  }
  blockNightUntilMs = millis() + NIGHT_BLOCK_WINDOW_MS;
}

// -------------------- Переход в AP --------------------
void wifiToAP() {
  WiFi.disconnect(true, true);
  delay(200);
  startConfigPortal();
}
