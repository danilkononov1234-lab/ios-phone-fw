// iOS-style "phone" for LilyGo T-Display S3 Touch + CC1101 + IR transmitter
// Libraries: LovyanGFX, RadioLib, IRremoteESP8266
// Board: ESP32S3 Dev Module, USB CDC On Boot: Enabled, Flash 16MB, PSRAM: OPI PSRAM,
//        Partition: 16M Flash (3MB APP/9.9MB FATFS)
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <LovyanGFX.hpp>
#include <RadioLib.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <NimBLEDevice.h>

// ======================= PINS =======================
// Touch version uses 16/17/18/21 for the touch controller, so CC1101 GDO0 and IR
// are moved to free header pins. Change here if your wiring differs.
#define PIN_LCD_POWER 15
#define PIN_BAT_ADC   4
#define TOUCH_SDA 18
#define TOUCH_SCL 17
#define TOUCH_RST 21
#define TOUCH_ADDR 0x15

#define CC_SCK  12
#define CC_MISO 13
#define CC_MOSI 11
#define CC_CS   2
#define CC_GDO0 10
#define IR_PIN  1

// ======================= DISPLAY =======================
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_Parallel8 _bus;
  lgfx::Light_PWM _light;
public:
  LGFX() {
    { auto c = _bus.config();
      c.port = 0; c.freq_write = 20000000;
      c.pin_wr = 8; c.pin_rd = 9; c.pin_rs = 7;
      c.pin_d0 = 39; c.pin_d1 = 40; c.pin_d2 = 41; c.pin_d3 = 42;
      c.pin_d4 = 45; c.pin_d5 = 46; c.pin_d6 = 47; c.pin_d7 = 48;
      _bus.config(c); _panel.setBus(&_bus); }
    { auto c = _panel.config();
      c.pin_cs = 6; c.pin_rst = 5; c.pin_busy = -1;
      c.panel_width = 170; c.panel_height = 320;
      c.memory_width = 240; c.memory_height = 320;
      c.offset_x = 35; c.offset_y = 0;
      c.readable = false; c.invert = true; c.rgb_order = false;
      c.dlen_16bit = false; c.bus_shared = false;
      _panel.config(c); }
    { auto c = _light.config();
      c.pin_bl = 38; c.invert = false; c.freq = 44100; c.pwm_channel = 7;
      _light.config(c); _panel.setLight(&_light); }
    setPanel(&_panel);
  }
};
LGFX lcd;
LGFX_Sprite cv(&lcd);

// ======================= RADIO / IR =======================
SPIClass radioSPI(HSPI);
CC1101 radio = new Module(CC_CS, CC_GDO0, RADIOLIB_NC, RADIOLIB_NC, radioSPI);
IRsend irsend(IR_PIN);
bool ccOk = false; int ccErr = 0;

// ======================= STATE =======================
enum Scr { S_LOCK, S_HOME, S_SUB, S_IR, S_WIFI, S_SET, S_CHAM, S_TVBG, S_GAME };
Scr scr = S_LOCK;
bool dirty = true;
uint32_t lastDraw = 0;
uint8_t bright = 200;
uint32_t daySec = 12 * 3600, baseMs = 0;

// Wallpapers: pairs of gradient colors used for lock/home background
struct Wall { uint8_t r1,g1,b1,r2,g2,b2; const char* name; };
const Wall walls[] = {
  {20,40,110, 120,60,160, "Dusk"},
  {10,10,20,  60,20,90,   "Midnight"},
  {255,120,60,255,200,90, "Sunset"},
  {10,90,70,  40,170,130, "Forest"},
  {30,30,35,  70,70,80,   "Graphite"},
};
const int N_WALLS = 5;
int wallIdx = 0;

uint32_t nowSec() { return (daySec + (millis() - baseMs) / 1000) % 86400; }

// Sub-GHz
float freq = 433.92;
const float presets[] = {315.0, 433.92, 868.35, 915.0};
int presetIdx = 1;
enum SgMode { SG_IDLE, SG_RX, SG_REC };
SgMode sg = SG_IDLE;
float rssi = -110, rssiPeak = -110;
uint32_t lastRssi = 0;
String sgStatus = "Ready";
#define MAXP 600
volatile uint32_t pulses[MAXP];
volatile int nPulse = 0;
volatile bool recStarted = false;
volatile uint32_t lastEdge = 0;
volatile uint8_t firstLevel = 0;

void IRAM_ATTR gdoIsr() {
  uint32_t t = micros();
  if (!recStarted) { recStarted = true; firstLevel = digitalRead(CC_GDO0); lastEdge = t; return; }
  if (nPulse < MAXP) { pulses[nPulse++] = t - lastEdge; lastEdge = t; }
}

// IR
String irStatus = "Ready";
const char* irNames[] = {"Sams PWR", "Sams V+", "Sams V-", "LG PWR", "LG V+", "LG V-", "Sony PWR", "Sony V+"};

// WiFi
int wifiN = 0;

// ======================= TOUCH =======================
bool readTouch(int &x, int &y) {
  Wire.beginTransmission(TOUCH_ADDR); Wire.write(0x02);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(TOUCH_ADDR, 5) < 5) return false;
  uint8_t n = Wire.read(), xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();
  if (!n) return false;
  x = ((xh & 0x0F) << 8) | xl; y = ((yh & 0x0F) << 8) | yl;
  return true;
}
bool hit(int tx, int ty, int x, int y, int w, int h) { return tx >= x && tx < x + w && ty >= y && ty < y + h; }

// ======================= UI HELPERS =======================
uint16_t C(uint8_t r, uint8_t g, uint8_t b) { return cv.color565(r, g, b); }

void wallGradient() {
  const Wall &w = walls[wallIdx];
  for (int y = 0; y < 320; y++) {
    cv.drawFastHLine(0, y, 170, C(w.r1 + (w.r2 - w.r1) * y / 320, w.g1 + (w.g2 - w.g1) * y / 320, w.b1 + (w.b2 - w.b1) * y / 320));
  }
}
void gradient(uint16_t a, uint16_t b) {
  uint8_t ar = (a >> 11) << 3, ag = ((a >> 5) & 63) << 2, ab = (a & 31) << 3;
  uint8_t br = (b >> 11) << 3, bg = ((b >> 5) & 63) << 2, bb = (b & 31) << 3;
  for (int y = 0; y < 320; y++) {
    cv.drawFastHLine(0, y, 170, C(ar + (br - ar) * y / 320, ag + (bg - ag) * y / 320, ab + (bb - ab) * y / 320));
  }
}

int batPercent() {
  int mv = analogReadMilliVolts(PIN_BAT_ADC) * 2;
  return constrain((mv - 3300) * 100 / 900, 0, 100);
}

void statusBar(uint16_t col = 0xFFFF) {
  uint32_t s = nowSec();
  char t[8]; snprintf(t, sizeof t, "%02u:%02u", s / 3600, (s / 60) % 60);
  cv.setFont(&fonts::Font2); cv.setTextColor(col);
  cv.setTextDatum(textdatum_t::middle_left); cv.drawString(t, 10, 11);
  // battery
  int p = batPercent();
  cv.drawRoundRect(134, 5, 24, 11, 3, col);
  cv.fillRect(158, 8, 2, 5, col);
  cv.fillRoundRect(136, 7, (20 * p) / 100, 7, 2, p > 20 ? C(80, 220, 100) : C(240, 70, 60));
  // signal bars
  for (int i = 0; i < 4; i++) cv.fillRect(100 + i * 5, 14 - (i + 1) * 2 - 1, 3, (i + 1) * 2 + 1, col);
}

void homeBar(uint16_t col = 0xFFFF) { cv.fillRoundRect(55, 312, 60, 4, 2, col); }

void btn(int x, int y, int w, int h, const char* t, uint16_t col, uint16_t tc = 0xFFFF) {
  cv.fillRoundRect(x, y, w, h, 10, col);
  cv.setFont(&fonts::Font2); cv.setTextColor(tc);
  cv.setTextDatum(textdatum_t::middle_center); cv.drawString(t, x + w / 2, y + h / 2);
}

void appHeader(const char* title) {
  cv.fillScreen(C(0, 0, 0));
  statusBar();
  cv.setFont(&fonts::Font2); cv.setTextColor(C(10, 132, 255));
  cv.setTextDatum(textdatum_t::middle_left); cv.drawString("< Home", 8, 34);
  cv.setTextColor(0xFFFF); cv.setTextDatum(textdatum_t::middle_center); cv.drawString(title, 110, 34);
  cv.drawFastHLine(0, 46, 170, C(50, 50, 55));
}

// ---- icons
void iconBase(int x, int y, uint16_t c1) { cv.fillRoundRect(x, y, 40, 40, 10, c1); }
void drawIcon(int id, int x, int y) {
  switch (id) {
    case 0: iconBase(x, y, C(255, 149, 0));   // Sub-GHz
      for (int r = 6; r <= 18; r += 6) cv.drawArc(x + 10, y + 30, r, r + 2, 270, 360, 0xFFFF);
      cv.fillCircle(x + 10, y + 30, 3, 0xFFFF); break;
    case 1: iconBase(x, y, C(255, 59, 48));   // IR
      cv.fillCircle(x + 20, y + 20, 9, 0xFFFF); cv.fillCircle(x + 20, y + 20, 4, C(255, 59, 48));
      cv.drawCircle(x + 20, y + 20, 14, 0xFFFF); break;
    case 2: iconBase(x, y, C(10, 132, 255));  // WiFi
      for (int r = 6; r <= 18; r += 6) cv.drawArc(x + 20, y + 30, r, r + 2, 225, 315, 0xFFFF);
      cv.fillCircle(x + 20, y + 30, 3, 0xFFFF); break;
    case 3: iconBase(x, y, C(142, 142, 147)); // Settings
      cv.fillCircle(x + 20, y + 20, 12, C(60, 60, 65)); cv.fillCircle(x + 20, y + 20, 5, C(142, 142, 147)); break;
    case 4: iconBase(x, y, C(52, 199, 89));   // Phone
      cv.fillCircle(x + 20, y + 20, 9, 0xFFFF); break;
    case 5: iconBase(x, y, C(0, 122, 255));   // Safari
      cv.drawCircle(x + 20, y + 20, 13, 0xFFFF); cv.fillTriangle(x + 14, y + 26, x + 17, y + 17, x + 26, y + 14, 0xFFFF); break;
    case 6: iconBase(x, y, C(48, 209, 88));   // Messages
      cv.fillEllipse(x + 20, y + 19, 13, 9, 0xFFFF); break;
    case 7: iconBase(x, y, C(0, 160, 150));   // Chameleon (card)
      cv.fillRoundRect(x + 8, y + 12, 24, 16, 4, 0xFFFF); cv.fillCircle(x + 25, y + 20, 3, C(0, 160, 150)); break;
    case 8: iconBase(x, y, C(28, 28, 30));     // TV-B-Gone
      cv.fillRoundRect(x + 7, y + 9, 26, 17, 3, 0xFFFF); cv.fillRect(x + 18, y + 26, 4, 5, 0xFFFF); cv.fillRect(x + 12, y + 31, 16, 3, 0xFFFF); break;
    case 9: iconBase(x, y, C(90, 60, 200));    // Snake / game
      cv.fillRoundRect(x + 7, y + 14, 26, 14, 6, 0xFFFF); cv.fillCircle(x + 14, y + 21, 2, C(90, 60, 200)); cv.fillCircle(x + 26, y + 21, 2, C(90, 60, 200)); break;
  }
}

// ======================= SCREENS =======================
const int APPS = 7;
const char* appNames[] = {"Sub-GHz", "Infrared", "Wi-Fi", "Settings", "Chameleon", "TV-B-Gone", "Snake"};
Scr appTargets[] = {S_SUB, S_IR, S_WIFI, S_SET, S_CHAM, S_TVBG, S_GAME};
const int appIcon[] = {0, 1, 2, 3, 7, 8, 9};
int appX(int i) { return 13 + (i % 3) * 53; }
int appY(int i) { return 40 + (i / 3) * 72; }

void drawLock() {
  wallGradient();
  statusBar();
  uint32_t s = nowSec();
  char t[8]; snprintf(t, sizeof t, "%u:%02u", s / 3600, (s / 60) % 60);
  cv.setTextColor(0xFFFF); cv.setTextDatum(textdatum_t::middle_center);
  cv.setFont(&fonts::Font2); cv.drawString("iPhone", 85, 50);
  cv.setFont(&fonts::FreeSansBold24pt7b); cv.drawString(t, 85, 105);
  cv.setFont(&fonts::Font2); cv.drawString("Swipe up to unlock", 85, 280);
  cv.fillCircle(25, 285, 16, C(0, 0, 0)); cv.fillCircle(145, 285, 16, C(0, 0, 0));
  homeBar();
}

void drawHome() {
  wallGradient();
  statusBar();
  for (int i = 0; i < APPS; i++) {
    drawIcon(appIcon[i], appX(i), appY(i));
    cv.setFont(&fonts::Font0); cv.setTextColor(0xFFFF);
    cv.setTextDatum(textdatum_t::top_center); cv.drawString(appNames[i], appX(i) + 20, appY(i) + 44);
  }
  cv.fillRoundRect(6, 245, 158, 58, 16, C(200, 200, 220));
  for (int i = 0; i < 3; i++) drawIcon(4 + i, 13 + i * 53, 254);
  homeBar();
}

void drawSub() {
  appHeader("Sub-GHz");
  cv.setTextColor(0xFFFF); cv.setTextDatum(textdatum_t::middle_center);
  cv.setFont(&fonts::FreeSansBold12pt7b);
  char f[20]; snprintf(f, sizeof f, "%.2f MHz", freq); cv.drawString(f, 85, 72);
  btn(8, 92, 36, 30, "-", C(58, 58, 62)); btn(50, 92, 70, 30, "Band", C(58, 58, 62)); btn(126, 92, 36, 30, "+", C(58, 58, 62));
  btn(8, 132, 50, 34, "Listen", sg == SG_RX ? C(52, 199, 89) : C(10, 132, 255));
  btn(60, 132, 50, 34, "Rec", sg == SG_REC ? C(255, 59, 48) : C(10, 132, 255));
  btn(112, 132, 50, 34, "Replay", nPulse >= 8 ? C(255, 149, 0) : C(70, 70, 75));
  // RSSI bar
  int w = constrain((int)(rssi + 110) * 150 / 80, 0, 150);
  cv.drawRoundRect(8, 180, 154, 14, 4, C(120, 120, 125));
  cv.fillRoundRect(10, 182, w, 10, 3, C(52, 199, 89));
  int pk = constrain((int)(rssiPeak + 110) * 150 / 80, 0, 150);
  cv.drawFastVLine(10 + pk, 180, 14, C(255, 204, 0));
  cv.setFont(&fonts::Font2); cv.setTextColor(0xFFFF);
  char r[32]; snprintf(r, sizeof r, "RSSI %d dBm", (int)rssi); cv.drawString(r, 85, 208);
  cv.setTextColor(C(180, 180, 185));
  cv.drawString(sgStatus, 85, 232);
  char p[32]; snprintf(p, sizeof p, "Pulses: %d", nPulse); cv.drawString(p, 85, 252);
  if (!ccOk) { cv.setTextColor(C(255, 59, 48)); char e[32]; snprintf(e, sizeof e, "CC1101 err %d", ccErr); cv.drawString(e, 85, 276); }
  cv.setFont(&fonts::Font0); cv.setTextColor(C(120, 120, 125));
  cv.drawString("Use only on your own devices", 85, 296);
  homeBar();
}

void drawIR() {
  appHeader("Infrared");
  for (int i = 0; i < 8; i++) btn(8 + (i % 2) * 80, 56 + (i / 2) * 44, 74, 36, irNames[i], C(58, 58, 62));
  cv.setFont(&fonts::Font2); cv.setTextColor(C(180, 180, 185)); cv.setTextDatum(textdatum_t::middle_center);
  cv.drawString(irStatus, 85, 250);
  homeBar();
}

void drawWifi() {
  appHeader("Wi-Fi");
  cv.setFont(&fonts::Font0); cv.setTextDatum(textdatum_t::middle_left);
  if (wifiN <= 0) { cv.setTextColor(0xFFFF); cv.drawString("No networks", 10, 70); }
  for (int i = 0; i < min(wifiN, 14); i++) {
    String s = WiFi.SSID(i); if (s.length() > 18) s = s.substring(0, 18); if (!s.length()) s = "(hidden)";
    cv.setTextColor(0xFFFF); cv.drawString(s, 8, 60 + i * 18);
    cv.setTextColor(C(150, 150, 155)); char r[12]; snprintf(r, sizeof r, "%d", WiFi.RSSI(i));
    cv.setTextDatum(textdatum_t::middle_right); cv.drawString(r, 162, 60 + i * 18); cv.setTextDatum(textdatum_t::middle_left);
  }
  btn(40, 280, 90, 26, "Rescan", C(10, 132, 255));
  homeBar();
}

void drawSet() {
  appHeader("Settings");
  cv.setFont(&fonts::Font2); cv.setTextColor(0xFFFF); cv.setTextDatum(textdatum_t::middle_left);
  cv.drawString("Brightness", 10, 60);
  btn(10, 72, 32, 26, "-", C(58, 58, 62)); btn(128, 72, 32, 26, "+", C(58, 58, 62));
  cv.fillRoundRect(48, 80, 70, 10, 4, C(58, 58, 62));
  cv.fillRoundRect(48, 80, bright * 70 / 255, 10, 4, C(10, 132, 255));
  cv.drawString("Wallpaper", 10, 114);
  btn(10, 126, 32, 26, "<", C(58, 58, 62)); btn(128, 126, 32, 26, ">", C(58, 58, 62));
  const Wall &w = walls[wallIdx];
  cv.fillRoundRect(48, 126, 74, 26, 6, C(w.r1, w.g1, w.b1));
  cv.setTextDatum(textdatum_t::middle_center); cv.setFont(&fonts::Font0); cv.setTextColor(0xFFFF);
  cv.drawString(w.name, 85, 139);
  cv.setFont(&fonts::Font2); cv.setTextDatum(textdatum_t::middle_left); cv.setTextColor(0xFFFF);
  cv.drawString("Time", 10, 172);
  uint32_t s = nowSec(); char t[8]; snprintf(t, sizeof t, "%02u:%02u", s / 3600, (s / 60) % 60);
  cv.setTextDatum(textdatum_t::middle_center); cv.setFont(&fonts::FreeSansBold12pt7b); cv.drawString(t, 85, 205);
  btn(10, 222, 70, 28, "Hour +", C(58, 58, 62)); btn(90, 222, 70, 28, "Min +", C(58, 58, 62));
  cv.setFont(&fonts::Font0); cv.setTextColor(C(150, 150, 155));
  char b[40]; snprintf(b, sizeof b, "Battery %d%%", batPercent()); cv.drawString(b, 85, 272);
  homeBar();
}

// ======================= CHAMELEON ULTRA (BLE / Nordic UART) =======================
// Frame: 11 EF | cmd(2) | status(2) | len(2) | LRC2 | data | LRC3   (big-endian)
static NimBLEUUID NUS_SVC("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
static NimBLEUUID NUS_RX("6e400002-b5a3-f393-e0a9-e50e24dcca9e");  // write to device
static NimBLEUUID NUS_TX("6e400003-b5a3-f393-e0a9-e50e24dcca9e");  // notifications from device
NimBLEClient* chClient = nullptr;
NimBLERemoteCharacteristic* chW = nullptr;
NimBLEAddress* chAddr = nullptr;
volatile bool chFound = false;
bool chConn = false;
int chSlot = 0;
String chStatus = "Not connected";
String chLines[4];  // 0 version, 1 battery, 2 HF scan, 3 LF scan
uint8_t chBuf[300];
volatile size_t chLen = 0;
portMUX_TYPE chMux = portMUX_INITIALIZER_UNLOCKED;

String hexs(const uint8_t* d, size_t n) {
  String s; char t[4];
  for (size_t i = 0; i < n; i++) { snprintf(t, sizeof t, "%02X", d[i]); s += t; }
  return s;
}

class ChScanCb : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* d) override {
    String n = d->getName().c_str();
    if (n.indexOf("Chameleon") >= 0 || n.indexOf("Ultra") >= 0) {
      delete chAddr; chAddr = new NimBLEAddress(d->getAddress()); chFound = true; NimBLEDevice::getScan()->stop();
    }
  }
} chScanCb;

void chNotify(NimBLERemoteCharacteristic*, uint8_t* d, size_t n, bool) {
  portENTER_CRITICAL(&chMux);
  if (chLen + n <= sizeof chBuf) { memcpy(chBuf + chLen, d, n); chLen += n; }
  portEXIT_CRITICAL(&chMux);
}

bool chSend(uint16_t cmd, const uint8_t* data = nullptr, uint8_t n = 0) {
  if (!chConn || !chW || n > 32) return false;
  uint8_t f[10 + 32];
  f[0] = 0x11; f[1] = 0xEF; f[2] = cmd >> 8; f[3] = cmd & 0xFF;
  f[4] = 0; f[5] = 0; f[6] = 0; f[7] = n;
  f[8] = (0x100 - (f[2] + f[3] + f[4] + f[5] + f[6] + f[7])) & 0xFF;
  uint8_t sum = 0;
  for (int i = 0; i < n; i++) { f[9 + i] = data[i]; sum += data[i]; }
  f[9 + n] = (0x100 - sum) & 0xFF;
  return chW->writeValue(f, 10 + n, false);
}

void drawCham();
void unlockAnimation();
void showBusy(const char* msg) { chStatus = msg; drawCham(); cv.pushSprite(&lcd, 0, 0); }

void chDisconnect() {
  if (chClient) { if (chClient->isConnected()) chClient->disconnect(); NimBLEDevice::deleteClient(chClient); chClient = nullptr; }
  chConn = false; chW = nullptr; chStatus = "Disconnected";
}

void chConnect() {
  showBusy("Scanning BLE...");
  chFound = false; chLen = 0;
  NimBLEScan* sc = NimBLEDevice::getScan();
  sc->setAdvertisedDeviceCallbacks(&chScanCb, false);
  sc->setActiveScan(true); sc->setInterval(100); sc->setWindow(99);
  sc->start(8, false);
  sc->clearResults();
  if (!chFound) { chStatus = "Chameleon not found"; return; }
  showBusy("Connecting...");
  chClient = NimBLEDevice::createClient();
  if (!chClient->connect(*chAddr)) { chStatus = "Connect failed"; NimBLEDevice::deleteClient(chClient); chClient = nullptr; return; }
  NimBLERemoteService* svc = chClient->getService(NUS_SVC);
  NimBLERemoteCharacteristic* tx = svc ? svc->getCharacteristic(NUS_TX) : nullptr;
  chW = svc ? svc->getCharacteristic(NUS_RX) : nullptr;
  if (!tx || !chW || !tx->subscribe(true, chNotify)) { chStatus = "No UART service"; chDisconnect(); return; }
  chConn = true; chStatus = "Connected";
  chSend(1000);  // read firmware version right away
}

void chHandle(uint16_t cmd, uint16_t st, const uint8_t* d, size_t n) {
  char b[48];
  switch (cmd) {
    case 1000: if (n >= 2) { snprintf(b, sizeof b, "FW v%u.%u", d[0], d[1]); chLines[0] = b; } break;
    case 1025: if (n >= 3) { snprintf(b, sizeof b, "Bat %u%%  %umV", d[2], (d[0] << 8) | d[1]); chLines[1] = b; } break;
    case 2000:
      if (n >= 1 && d[0] > 0 && n >= (size_t)(d[0] + 4)) {
        uint8_t u = d[0]; chLines[2] = "HF " + hexs(d + 1, u);
        snprintf(b, sizeof b, " SAK %02X", d[1 + u + 2]); chLines[2] += b;
      } else chLines[2] = "HF: no tag";
      break;
    case 3000: chLines[3] = (n >= 5) ? "LF " + hexs(d, 5) : String("LF: no tag"); break;
    case 1003: chStatus = "Slot set"; break;
    default: snprintf(b, sizeof b, "cmd %u st %04X", cmd, st); chStatus = b; break;
  }
  dirty = true;
}

void chService() {
  if (chClient && chConn && !chClient->isConnected()) { chConn = false; chStatus = "Disconnected"; dirty = true; }
  for (;;) {
    uint8_t f[300]; size_t total = 0;
    portENTER_CRITICAL(&chMux);
    while (chLen > 0 && chBuf[0] != 0x11) { memmove(chBuf, chBuf + 1, --chLen); }  // resync
    if (chLen >= 9) {
      size_t len = (chBuf[6] << 8) | chBuf[7];
      if (len > 280) { chLen = 0; }
      else if (chLen >= 10 + len) { total = 10 + len; memcpy(f, chBuf, total); chLen -= total; memmove(chBuf, chBuf + total, chLen); }
    }
    portEXIT_CRITICAL(&chMux);
    if (!total) break;
    chHandle((f[2] << 8) | f[3], (f[4] << 8) | f[5], f + 9, total - 10);
  }
}

void drawCham() {
  appHeader("Chameleon");
  bool on = chConn;
  uint16_t g = C(58, 58, 62), off = C(40, 40, 44);
  btn(8, 54, 154, 30, on ? "Disconnect" : "Connect", on ? C(255, 59, 48) : C(10, 132, 255));
  btn(8, 92, 74, 30, "Version", on ? g : off); btn(88, 92, 74, 30, "Battery", on ? g : off);
  btn(8, 128, 74, 30, "Scan HF", on ? C(255, 149, 0) : off); btn(88, 128, 74, 30, "Scan LF", on ? C(255, 149, 0) : off);
  char sl[16]; snprintf(sl, sizeof sl, "Slot %d", chSlot + 1);
  btn(8, 164, 36, 30, "-", on ? g : off); btn(126, 164, 36, 30, "+", on ? g : off);
  cv.setFont(&fonts::Font2); cv.setTextColor(0xFFFF); cv.setTextDatum(textdatum_t::middle_center); cv.drawString(sl, 85, 179);
  cv.setTextColor(C(180, 180, 185)); cv.drawString(chStatus, 85, 212);
  cv.setFont(&fonts::Font0); cv.setTextColor(0xFFFF); cv.setTextDatum(textdatum_t::middle_left);
  for (int i = 0; i < 4; i++) cv.drawString(chLines[i], 8, 238 + i * 15);
  homeBar();
}

void chTap(int x, int y) {
  if (hit(x, y, 8, 54, 154, 30)) { if (chConn) chDisconnect(); else chConnect(); return; }
  if (!chConn) return;
  if (hit(x, y, 8, 92, 74, 30))   chSend(1000);
  if (hit(x, y, 88, 92, 74, 30))  chSend(1025);
  if (hit(x, y, 8, 128, 74, 30))  { chLines[2] = "HF: scanning..."; chSend(2000); }
  if (hit(x, y, 88, 128, 74, 30)) { chLines[3] = "LF: scanning..."; chSend(3000); }
  if (hit(x, y, 8, 164, 36, 30) || hit(x, y, 126, 164, 36, 30)) {
    chSlot = (chSlot + (x < 85 ? 7 : 1)) % 8;
    uint8_t s = chSlot; chSend(1003, &s, 1);
  }
}

// ======================= TV-B-GONE =======================
// Compact power-toggle codes (protocol, addr/cmd or raw) for common TV brands.
// This only toggles power on IR-receiving devices in line of sight of the emitter.
struct TvCode { uint8_t type; uint32_t a; uint16_t b; }; // type 0=NEC 1=Samsung 2=Sony 3=RC5(as raw addr/cmd)
const TvCode tvCodes[] = {
  {0, 0x20DF10EF, 32}, {0, 0x40BF00FF, 32}, {0, 0x08F7807F, 32}, {0, 0xA25D708F, 32}, // various NEC TVs
  {1, 0xE0E040BF, 32}, {1, 0xE0E09966, 32},
  {2, 0xA90, 12}, {2, 0x490, 12}, {2, 0xC30, 12},
  {0, 0x00FF00FF, 32}, {0, 0xFF00FF00, 32}, {0, 0x01FE906F, 32},
};
const int N_TVCODES = 12;
bool tvbRunning = false;
int tvbIdx = 0;
uint32_t tvbLast = 0;
String tvbStatus = "Idle";

void tvbSendOne() {
  const TvCode &c = tvCodes[tvbIdx];
  switch (c.type) {
    case 0: irsend.sendNEC(c.a, c.b); break;
    case 1: irsend.sendSAMSUNG(c.a, c.b); break;
    case 2: irsend.sendSony(c.a, c.b, 2); break;
  }
  tvbStatus = "Sent " + String(tvbIdx + 1) + "/" + String(N_TVCODES);
  tvbIdx = (tvbIdx + 1) % N_TVCODES;
}
void tvbService() {
  if (!tvbRunning) return;
  if (millis() - tvbLast > 180) { tvbLast = millis(); tvbSendOne(); dirty = true; if (tvbIdx == 0) { tvbRunning = false; tvbStatus = "Done - cycled all codes"; } }
}
void drawTvb() {
  appHeader("TV-B-Gone");
  cv.setFont(&fonts::Font2); cv.setTextColor(0xFFFF); cv.setTextDatum(textdatum_t::middle_center);
  cv.drawString("Point at your TV", 85, 80);
  btn(25, 110, 120, 44, tvbRunning ? "Stop" : "Start", tvbRunning ? C(255, 59, 48) : C(52, 199, 89));
  cv.setFont(&fonts::Font0); cv.setTextColor(C(180, 180, 185));
  cv.drawString(tvbStatus, 85, 175);
  cv.drawString("Cycles common TV off-codes", 85, 220);
  cv.drawString("Use on your own TV only", 85, 238);
  homeBar();
}
void tvbTap(int x, int y) {
  if (hit(x, y, 25, 110, 120, 44)) {
    tvbRunning = !tvbRunning;
    if (tvbRunning) { tvbIdx = 0; tvbLast = 0; tvbStatus = "Running..."; } else tvbStatus = "Stopped";
  }
}

// ======================= SNAKE =======================
#define SNK_W 17
#define SNK_H 24
uint8_t snkX[200], snkY[200];
int snkLen = 3, snkDirX = 1, snkDirY = 0, snkFoodX = 8, snkFoodY = 8;
bool snkOver = false;
uint32_t snkLast = 0;
int snkScore = 0;

void snkReset() {
  snkLen = 3; snkDirX = 1; snkDirY = 0; snkOver = false; snkScore = 0;
  for (int i = 0; i < snkLen; i++) { snkX[i] = 5 - i; snkY[i] = 10; }
  snkFoodX = random(1, SNK_W - 1); snkFoodY = random(1, SNK_H - 1);
}
void snkStep() {
  if (snkOver) return;
  for (int i = snkLen; i > 0; i--) { snkX[i] = snkX[i - 1]; snkY[i] = snkY[i - 1]; }
  snkX[0] += snkDirX; snkY[0] += snkDirY;
  if (snkX[0] >= SNK_W) snkX[0] = 0; if (snkX[0] < 0) snkX[0] = SNK_W - 1;
  if (snkY[0] >= SNK_H) snkY[0] = 0; if (snkY[0] < 0) snkY[0] = SNK_H - 1;
  for (int i = 1; i < snkLen; i++) if (snkX[i] == snkX[0] && snkY[i] == snkY[0]) snkOver = true;
  if (snkX[0] == snkFoodX && snkY[0] == snkFoodY) {
    snkScore++; if (snkLen < 199) snkLen++;
    snkFoodX = random(1, SNK_W - 1); snkFoodY = random(1, SNK_H - 1);
  }
}
void snkService() {
  if (scr != S_GAME || snkOver) return;
  if (millis() - snkLast > 160) { snkLast = millis(); snkStep(); dirty = true; }
}
void drawGame() {
  cv.fillScreen(C(10, 10, 15));
  statusBar();
  cv.setFont(&fonts::Font2); cv.setTextColor(C(10, 132, 255)); cv.setTextDatum(textdatum_t::middle_left);
  cv.drawString("< Home", 8, 34);
  cv.setTextColor(0xFFFF); cv.setTextDatum(textdatum_t::middle_center);
  char sc[16]; snprintf(sc, sizeof sc, "Score %d", snkScore); cv.drawString(sc, 110, 34);
  int ox = 1, oy = 46, cell = 9;
  for (int i = 0; i < snkLen; i++) cv.fillRoundRect(ox + snkX[i] * cell, oy + snkY[i] * cell, cell - 1, cell - 1, 2, i == 0 ? C(52, 199, 89) : C(30, 140, 60));
  cv.fillRoundRect(ox + snkFoodX * cell, oy + snkFoodY * cell, cell - 1, cell - 1, 3, C(255, 59, 48));
  // on-screen D-pad
  int by = 262;
  btn(60, by, 50, 34, "Up", C(44, 44, 48));
  btn(10, by + 38, 50, 34, "Left", C(44, 44, 48));
  btn(110, by + 38, 50, 34, "Right", C(44, 44, 48));
  btn(60, by + 38, 50, 34, "Down", C(44, 44, 48));
  if (snkOver) {
    cv.fillRoundRect(25, 120, 120, 60, 10, C(0, 0, 0));
    cv.setTextColor(0xFFFF); cv.drawString("Game Over", 85, 142);
    cv.setFont(&fonts::Font0); cv.drawString("Tap to restart", 85, 162);
  }
}
void gameTap(int x, int y) {
  if (snkOver) { snkReset(); return; }
  int by = 262;
  if (hit(x, y, 60, by, 50, 34) && snkDirY == 0) { snkDirX = 0; snkDirY = -1; }
  else if (hit(x, y, 60, by + 38, 50, 34) && snkDirY == 0) { snkDirX = 0; snkDirY = 1; }
  else if (hit(x, y, 10, by + 38, 50, 34) && snkDirX == 0) { snkDirX = -1; snkDirY = 0; }
  else if (hit(x, y, 110, by + 38, 50, 34) && snkDirX == 0) { snkDirX = 1; snkDirY = 0; }
}

void render() {
  switch (scr) {
    case S_LOCK: drawLock(); break; case S_HOME: drawHome(); break; case S_SUB: drawSub(); break;
    case S_IR: drawIR(); break; case S_WIFI: drawWifi(); break; case S_SET: drawSet(); break; case S_CHAM: drawCham(); break;
  }
  cv.pushSprite(&lcd, 0, 0);
}

// ======================= SUB-GHZ LOGIC =======================
void sgStop() {
  detachInterrupt(digitalPinToInterrupt(CC_GDO0));
  if (ccOk) radio.standby();
  sg = SG_IDLE;
}
bool sgTune() {
  if (!ccOk) { sgStatus = "No CC1101"; return false; }
  radio.standby();
  int st = radio.setFrequency(freq);
  if (st != RADIOLIB_ERR_NONE) { sgStatus = "Freq out of range"; return false; }
  return true;
}
void sgStartRx(bool record) {
  sgStop();
  if (!sgTune()) return;
  radio.setOOK(true);
  radio.setRxBandwidth(203.125);
  radio.receiveDirect();
  pinMode(CC_GDO0, INPUT);
  rssiPeak = -110;
  if (record) {
    nPulse = 0; recStarted = false;
    attachInterrupt(digitalPinToInterrupt(CC_GDO0), gdoIsr, CHANGE);
    sg = SG_REC; sgStatus = "Waiting for signal...";
  } else { sg = SG_RX; sgStatus = "Listening"; }
}
void sgReplay() {
  if (nPulse < 8 || !ccOk) return;
  sgStop();
  if (!sgTune()) return;
  radio.setOOK(true);
  radio.setOutputPower(10);
  radio.transmitDirect();
  pinMode(CC_GDO0, OUTPUT);
  for (int rep = 0; rep < 3; rep++) {
    for (int i = 0; i < nPulse; i++) { digitalWrite(CC_GDO0, firstLevel ^ (i & 1)); delayMicroseconds(pulses[i]); }
    digitalWrite(CC_GDO0, LOW); delay(20);
  }
  radio.standby(); pinMode(CC_GDO0, INPUT);
  sgStatus = "Sent x3";
}
void sgService() {
  if (scr != S_SUB || sg == SG_IDLE) return;
  if (millis() - lastRssi > 100) {
    lastRssi = millis(); rssi = radio.getRSSI(); if (rssi > rssiPeak) rssiPeak = rssi;
  }
  if (sg == SG_REC) {
    if (nPulse >= MAXP || (recStarted && nPulse >= 24 && micros() - lastEdge > 40000)) {
      detachInterrupt(digitalPinToInterrupt(CC_GDO0)); radio.standby(); sg = SG_IDLE;
      sgStatus = "Captured " + String(nPulse) + " pulses";
    } else if (recStarted && nPulse < 24 && micros() - lastEdge > 100000) {
      nPulse = 0; recStarted = false;  // noise, restart
    }
  }
}

// ======================= IR LOGIC =======================
void irSend(int i) {
  switch (i) {
    case 0: irsend.sendSAMSUNG(0xE0E040BF, 32); break;
    case 1: irsend.sendSAMSUNG(0xE0E0E01F, 32); break;
    case 2: irsend.sendSAMSUNG(0xE0E0D02F, 32); break;
    case 3: irsend.sendNEC(0x20DF10EF, 32); break;
    case 4: irsend.sendNEC(0x20DF40BF, 32); break;
    case 5: irsend.sendNEC(0x20DFC03F, 32); break;
    case 6: irsend.sendSony(0xA90, 12, 2); break;
    case 7: irsend.sendSony(0x490, 12, 2); break;
  }
  irStatus = String("Sent: ") + irNames[i];
}

// ======================= WIFI =======================
void wifiScan() {
  cv.fillRect(0, 50, 170, 220, 0); cv.setFont(&fonts::Font2); cv.setTextColor(0xFFFF);
  cv.setTextDatum(textdatum_t::middle_center); cv.drawString("Scanning...", 85, 150); cv.pushSprite(&lcd, 0, 0);
  WiFi.mode(WIFI_STA); WiFi.disconnect();
  wifiN = WiFi.scanNetworks();
}

// ======================= NAVIGATION / TOUCH =======================
void goTo(Scr s) {
  if (scr == S_SUB && s != S_SUB) sgStop();
  scr = s; dirty = true;
  if (s == S_WIFI) { wifiScan(); }
}

void onTap(int x, int y) {
  dirty = true;
  if (scr == S_HOME) {
    for (int i = 0; i < APPS; i++) if (hit(x, y, appX(i), appY(i), 40, 56)) { goTo(appTargets[i]); return; }
    return;
  }
  if (scr == S_LOCK) return;
  if (hit(x, y, 0, 22, 60, 24)) { goTo(S_HOME); return; }
  if (scr == S_SUB) {
    if (hit(x, y, 8, 92, 36, 30))  { freq = max(300.0f, freq - 0.1f); if (sg != SG_IDLE) sgStartRx(sg == SG_REC); }
    if (hit(x, y, 126, 92, 36, 30)) { freq = min(928.0f, freq + 0.1f); if (sg != SG_IDLE) sgStartRx(sg == SG_REC); }
    if (hit(x, y, 50, 92, 70, 30))  { presetIdx = (presetIdx + 1) % 4; freq = presets[presetIdx]; if (sg != SG_IDLE) sgStartRx(sg == SG_REC); }
    if (hit(x, y, 8, 132, 50, 34))  { if (sg == SG_RX) { sgStop(); sgStatus = "Stopped"; } else sgStartRx(false); }
    if (hit(x, y, 60, 132, 50, 34)) { if (sg == SG_REC) { sgStop(); sgStatus = "Stopped"; } else sgStartRx(true); }
    if (hit(x, y, 112, 132, 50, 34)) sgReplay();
  } else if (scr == S_IR) {
    for (int i = 0; i < 8; i++) if (hit(x, y, 8 + (i % 2) * 80, 56 + (i / 2) * 44, 74, 36)) irSend(i);
  } else if (scr == S_WIFI) {
    if (hit(x, y, 40, 280, 90, 26)) wifiScan();
  } else if (scr == S_CHAM) {
    chTap(x, y);
  } else if (scr == S_TVBG) {
    tvbTap(x, y);
  } else if (scr == S_GAME) {
    gameTap(x, y);
  } else if (scr == S_SET) {
    if (hit(x, y, 10, 72, 32, 26))  { bright = max(10, bright - 25); lcd.setBrightness(bright); }
    if (hit(x, y, 128, 72, 32, 26)) { bright = min(255, bright + 25); lcd.setBrightness(bright); }
    if (hit(x, y, 10, 126, 32, 26))  { wallIdx = (wallIdx + N_WALLS - 1) % N_WALLS; }
    if (hit(x, y, 128, 126, 32, 26)) { wallIdx = (wallIdx + 1) % N_WALLS; }
    if (hit(x, y, 10, 222, 70, 28)) { daySec = (nowSec() + 3600) % 86400; baseMs = millis(); }
    if (hit(x, y, 90, 222, 70, 28)) { daySec = (nowSec() + 60) % 86400; baseMs = millis(); }
  }
}

void onRelease(int sx, int sy, int ex, int ey) {
  int dx = ex - sx, dy = ey - sy;
  if (dy < -50 && abs(dx) < 70) {                        // swipe up
    if (scr == S_LOCK) { unlockAnimation(); goTo(S_HOME); return; }
    if (sy > 270 && scr != S_HOME) { goTo(S_HOME); return; }
  }
  if (abs(dx) < 15 && abs(dy) < 15) onTap(ex, ey);
}

void handleTouch() {
  static bool down = false; static int sx, sy, lx, ly; static uint32_t last = 0;
  if (millis() - last < 12) return; last = millis();
  int x, y;
  if (readTouch(x, y)) { if (!down) { down = true; sx = x; sy = y; } lx = x; ly = y; }
  else if (down) { down = false; onRelease(sx, sy, lx, ly); }
}

// ======================= ANIMATIONS =======================
void bootAnimation() {
  for (int f = 0; f <= 20; f++) {
    cv.fillScreen(C(0, 0, 0));
    int r = f * 2;
    cv.fillCircle(85, 150, r, C(255, 255, 255));
    if (f > 12) { cv.setFont(&fonts::Font2); cv.setTextColor(C(0,0,0)); cv.setTextDatum(textdatum_t::middle_center); cv.drawString("iPhone", 85, 150); }
    cv.pushSprite(&lcd, 0, 0);
    delay(18);
  }
  delay(200);
}
void unlockAnimation() {
  for (int f = 0; f <= 10; f++) {
    int off = f * 32;
    cv.fillScreen(C(0, 0, 0));
    cv.pushSprite(&lcd, 0, -off);
    delay(8);
  }
}

// ======================= SETUP / LOOP =======================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LCD_POWER, OUTPUT); digitalWrite(PIN_LCD_POWER, HIGH);
  lcd.init(); lcd.setRotation(0); lcd.setBrightness(bright);
  cv.setColorDepth(16); cv.setPsram(true);
  if (!cv.createSprite(170, 320)) { cv.setPsram(false); cv.createSprite(170, 320); }

  pinMode(TOUCH_RST, OUTPUT); digitalWrite(TOUCH_RST, LOW); delay(10); digitalWrite(TOUCH_RST, HIGH); delay(60);
  Wire.begin(TOUCH_SDA, TOUCH_SCL); Wire.setClock(400000);

  radioSPI.begin(CC_SCK, CC_MISO, CC_MOSI, CC_CS);
  ccErr = radio.begin(433.92);
  ccOk = (ccErr == RADIOLIB_ERR_NONE);
  if (ccOk) { radio.setOOK(true); radio.standby(); }

  irsend.begin();
  NimBLEDevice::init("iPhone");
  baseMs = millis();
  bootAnimation();
}

void loop() {
  handleTouch();
  sgService();
  chService();
  tvbService();
  snkService();
  uint32_t period = (scr == S_SUB && sg != SG_IDLE) ? 120 : (scr == S_GAME ? 60 : 1000);
  if (dirty || millis() - lastDraw > period) { render(); lastDraw = millis(); dirty = false; }
  delay(4);
}
