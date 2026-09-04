/*
 * 003_esp_s3_touch_quadrants_on_serial
 *
 * Board: Waveshare ESP32-S3-Touch-ePaper-1.54
 *        (200x200 black/white e-paper, FT6336 capacitive touch, I2C addr 0x38)
 *
 * Touch-reception analogue of 002_esp_s3_two_buttons_on_serial: no e-paper
 * drawing, just serial. Reports raw touch (x,y) and flags two "virtual
 * buttons" -- the lower-left and lower-right quadrants of the panel.
 *
 * Arduino IDE settings:
 *   Board           : ESP32S3 Dev Module
 *   USB CDC On Boot : Enabled     (so Serial shows up over USB)
 *   PSRAM           : OPI PSRAM
 *   Flash Size      : 8MB         (this board has 8MB; 16MB bootloops on boot)
 *   Partition       : 8M with spiffs  (or any "8M" scheme)
 *   Monitor baud    : 115200
 *
 * FT6336 protocol (single point, polled):
 *   reg 0x02        : number of touch points (low nibble, 0..2)
 *   reg 0x03..0x06  : P1_XH, P1_XL, P1_YH, P1_YL
 *   x = ((XH & 0x0F) << 8) | XL ;  y = ((YH & 0x0F) << 8) | YL   (0..199)
 *
 * Pin map from Waveshare's user_config.h for this board.
 */

#include <Wire.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>

// ---- Pin map ----
static const int PIN_TP_SDA  = 47;
static const int PIN_TP_SCL  = 48;
static const int PIN_TP_INT  = 21;   // active low, falls on new touch (unused: we poll)
static const int PIN_TP_RST  = 7;
static const int PIN_EPD_PWR = 6;    // peripheral power rail, active LOW

// e-paper (SSD1681, 200x200 BW) -- pins from Waveshare user_config.h
static const int PIN_EPD_CS   = 11;
static const int PIN_EPD_DC   = 10;
static const int PIN_EPD_RST  = 9;
static const int PIN_EPD_BUSY = 8;
static const int PIN_EPD_SCK  = 12;
static const int PIN_EPD_MOSI = 13;

GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> display(
    GxEPD2_154_D67(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));

enum Zone { ZONE_NONE, ZONE_LL, ZONE_LR, ZONE_UPPER };
static Zone lastZone = ZONE_NONE;

// Partial-refresh the bottom strip with the latest touch result.
static void epdShow(const char *msg) {
  display.setPartialWindow(0, 140, 200, 60);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setCursor(6, 175);
    display.print(msg);
  } while (display.nextPage());
}

static const uint8_t FT6336_ADDR = 0x38;

// ---- Panel geometry ----
static const int W = 200, H = 200;
static const int MID_X = W / 2, MID_Y = H / 2;

// ---- Orientation calibration ----
// Touch the four corners once; if the reported quadrant is mirrored or
// rotated versus where you actually pressed, toggle these.
static const bool SWAP_XY = false;
static const bool FLIP_X  = false;
static const bool FLIP_Y  = false;

static void ft6336Reset() {
  pinMode(PIN_TP_RST, OUTPUT);
  digitalWrite(PIN_TP_RST, HIGH); delay(20);
  digitalWrite(PIN_TP_RST, LOW);  delay(20);
  digitalWrite(PIN_TP_RST, HIGH); delay(150);   // FT6336 needs ~100ms+ to boot
}

// Read `len` bytes starting at register `reg`. Returns true on success.
static bool ft6336Read(uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(FT6336_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;   // repeated start
  if (Wire.requestFrom((int)FT6336_ADDR, (int)len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

// Returns true if a finger is down; fills x,y in panel coords (0..199).
static bool readTouch(int &x, int &y) {
  uint8_t n = 0;
  if (!ft6336Read(0x02, &n, 1)) return false;
  if ((n & 0x0F) == 0) return false;

  uint8_t b[4];
  if (!ft6336Read(0x03, b, 4)) return false;   // XH, XL, YH, YL
  int rx = (((int)(b[0] & 0x0F)) << 8) | b[1];
  int ry = (((int)(b[2] & 0x0F)) << 8) | b[3];

  if (SWAP_XY) { int t = rx; rx = ry; ry = t; }
  if (FLIP_X) rx = (W - 1) - rx;
  if (FLIP_Y) ry = (H - 1) - ry;

  x = constrain(rx, 0, W - 1);
  y = constrain(ry, 0, H - 1);
  return true;
}

// Scan the I2C bus, print every address that ACKs. Returns device count.
static int i2cScan() {
  Serial.print("I2C scan:");
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf(" 0x%02X", a); found++; }
  }
  Serial.println(found ? "" : " (no devices!)");
  return found;
}

static Zone zoneOf(int x, int y) {
  if (y < MID_Y) return ZONE_UPPER;            // upper half -> not a button
  return (x < MID_X) ? ZONE_LL : ZONE_LR;      // lower half
}

static const char *zoneName(Zone z) {
  switch (z) {
    case ZONE_LL:    return "LOWER_LEFT";
    case ZONE_LR:    return "LOWER_RIGHT";
    case ZONE_UPPER: return "upper";
    default:         return "none";
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("003_esp_s3_touch_quadrants_on_serial");

  pinMode(PIN_EPD_PWR, OUTPUT);
  digitalWrite(PIN_EPD_PWR, LOW);   // rail ON (active low); harmless if not gated

  pinMode(PIN_TP_INT, INPUT_PULLUP);
  ft6336Reset();

  Wire.begin(PIN_TP_SDA, PIN_TP_SCL, 400000);

  // --- e-paper: draw a static screen so we can see the sketch is alive ---
  delay(100);
  SPI.begin(PIN_EPD_SCK, -1, PIN_EPD_MOSI, PIN_EPD_CS);
  display.init(0, true, 2, false);
  display.setRotation(0);
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_BLACK);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setCursor(6, 20);  display.print("003 touch test");
    display.setCursor(6, 45);  display.print("LL = BUTTON 1");
    display.setCursor(6, 65);  display.print("LR = BUTTON 2");
    display.drawLine(100, 90, 100, 199, GxEPD_BLACK);
    display.drawLine(0, 90, 199, 90, GxEPD_BLACK);
  } while (display.nextPage());
  epdShow("touch a quadrant");

  i2cScan();
  Serial.println("Touch lower-left = BUTTON 1, lower-right = BUTTON 2");
  // This board has no RST button, so the banner above is easy to miss.
  // The heartbeat below reprints scan + chip IDs every 2s.
}

void loop() {
  static uint32_t lastBeat = 0;
  int x, y;
  Zone z = ZONE_NONE;

  if (readTouch(x, y)) {
    z = zoneOf(x, y);
    if (z != lastZone) {
      Serial.printf("touch (%3d,%3d)  zone=%s", x, y, zoneName(z));
      if (z == ZONE_LL) Serial.print("   <<< BUTTON 1");
      if (z == ZONE_LR) Serial.print("   <<< BUTTON 2");
      Serial.println();
      if (z == ZONE_LL) epdShow("BUTTON 1");
      else if (z == ZONE_LR) epdShow("BUTTON 2");
      else epdShow("upper half");
    }
  } else if (lastZone != ZONE_NONE) {
    Serial.println("release");
    epdShow("(released)");
  }
  lastZone = z;

  // Heartbeat every 2s so an idle bus still produces output (no RST button
  // to re-trigger setup()). Prints bus scan, chip IDs, touch-count reg, INT.
  if (millis() - lastBeat > 2000) {
    lastBeat = millis();
    int n = i2cScan();
    uint8_t id[3] = {0xFF, 0xFF, 0xFF};
    ft6336Read(0xA3, &id[0], 1);   // chip id   (FT6336 -> 0x64)
    ft6336Read(0xA6, &id[1], 1);   // firmware id
    ft6336Read(0xA8, &id[2], 1);   // vendor id (FocalTech -> 0x11)
    uint8_t td = 0xFF;
    bool ok = ft6336Read(0x02, &td, 1);
    Serial.printf("[hb] devs=%d chipID=0x%02X fwID=0x%02X vendID=0x%02X  "
                  "reg0x02 %s val=0x%02X  INT=%d\n",
                  n, id[0], id[1], id[2],
                  ok ? "OK" : "FAIL", td, digitalRead(PIN_TP_INT));
  }

  delay(30);   // ~33 Hz poll; also debounces
}
