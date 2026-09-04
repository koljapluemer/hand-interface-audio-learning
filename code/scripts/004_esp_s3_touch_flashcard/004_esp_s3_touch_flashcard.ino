/*
 * 004_esp_s3_touch_flashcard
 *
 * Board: Waveshare ESP32-S3-Touch-ePaper-1.54
 *        (200x200 black/white e-paper, FT6336 capacitive touch, I2C addr 0x38)
 *
 * Same city->country flashcard game as 001_esp_c3_basic_flashcard, adapted
 * to the touch e-paper board's quadrant layout from
 * 003_esp_s3_touch_quadrants_on_serial:
 *
 *   - Prompt state: city name + "is in?" on top; the whole lower half
 *     (both quadrants together) is one "Reveal" button.
 *   - Answer state: country name on top; lower-left quadrant is "Wrong",
 *     lower-right quadrant is "Correct". Either one just advances to a new
 *     random card -- nothing is persisted.
 *
 * Arduino IDE settings: same as 003_esp_s3_touch_quadrants_on_serial.
 */

#include <Wire.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>

// ---- Pin map (from Waveshare user_config.h) ----
static const int PIN_TP_SDA  = 47;
static const int PIN_TP_SCL  = 48;
static const int PIN_TP_INT  = 21;   // active low, falls on new touch (unused: we poll)
static const int PIN_TP_RST  = 7;
static const int PIN_EPD_PWR = 6;    // peripheral power rail, active LOW

static const int PIN_EPD_CS   = 11;
static const int PIN_EPD_DC   = 10;
static const int PIN_EPD_RST  = 9;
static const int PIN_EPD_BUSY = 8;
static const int PIN_EPD_SCK  = 12;
static const int PIN_EPD_MOSI = 13;

GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> display(
    GxEPD2_154_D67(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));

static const uint8_t FT6336_ADDR = 0x38;

// ---- Panel geometry ----
static const int W = 200, H = 200;
static const int MID_X = W / 2, MID_Y = H / 2;

enum Zone { ZONE_NONE, ZONE_LL, ZONE_LR, ZONE_UPPER };

// ---- Orientation calibration (validated in 003) ----
static const bool SWAP_XY = false;
static const bool FLIP_X  = false;
static const bool FLIP_Y  = false;

// ---- Touch (FT6336) ----
static void ft6336Reset() {
  pinMode(PIN_TP_RST, OUTPUT);
  digitalWrite(PIN_TP_RST, HIGH); delay(20);
  digitalWrite(PIN_TP_RST, LOW);  delay(20);
  digitalWrite(PIN_TP_RST, HIGH); delay(150);   // FT6336 needs ~100ms+ to boot
}

static bool ft6336Read(uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(FT6336_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;   // repeated start
  if (Wire.requestFrom((int)FT6336_ADDR, (int)len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

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

static Zone zoneOf(int x, int y) {
  if (y < MID_Y) return ZONE_UPPER;            // upper half -> not a button
  return (x < MID_X) ? ZONE_LL : ZONE_LR;      // lower half
}

// ---- Flashcard data ----
struct CityCountry {
  const char *city;
  const char *country;
};

static const CityCountry PAIRS[] = {
  { "Ouani", "Comoros" },
  { "Mandza", "Comoros" },
  { "Fomboni", "Comoros" },
  { "Encamp", "Andorra" },
  { "La Massana", "Andorra" },
  { "Canillo", "Andorra" },
  { "Ordino", "Andorra" },
  { "Nuuk", "Greenland" },
  { "Sisimiut", "Greenland" },
  { "Ilulissat", "Greenland" },
  { "Aasiaat", "Greenland" },
};
static const int NUM_PAIRS = sizeof(PAIRS) / sizeof(PAIRS[0]);
static int currentIndex = 0;

// ---- Game state ----
enum GameState { STATE_PROMPT, STATE_ANSWER };
static GameState gameState = STATE_PROMPT;

// Print text centered on (cx, baselineY).
static void printCentered(int cx, int baselineY, const char *text) {
  int16_t bx, by;
  uint16_t bw, bh;
  display.getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
  display.setCursor(cx - bw / 2, baselineY);
  display.print(text);
}

static void drawPromptScreen() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    display.setFont(&FreeMonoBold12pt7b);
    display.setCursor(6, 40);
    display.print(PAIRS[currentIndex].city);

    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(6, 65);
    display.print("is in?");

    display.drawLine(0, MID_Y, W - 1, MID_Y, GxEPD_BLACK);
    display.drawRect(4, MID_Y + 4, W - 8, H - MID_Y - 8, GxEPD_BLACK);

    display.setFont(&FreeMonoBold12pt7b);
    printCentered(MID_X, 155, "Reveal");
  } while (display.nextPage());
}

static void drawAnswerScreen() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(6, 30);
    display.print(PAIRS[currentIndex].city);

    display.setFont(&FreeMonoBold12pt7b);
    display.setCursor(6, 65);
    display.print(PAIRS[currentIndex].country);

    display.drawLine(0, MID_Y, W - 1, MID_Y, GxEPD_BLACK);
    display.drawLine(MID_X, MID_Y, MID_X, H - 1, GxEPD_BLACK);
    display.drawRect(4, MID_Y + 4, MID_X - 8, H - MID_Y - 8, GxEPD_BLACK);
    display.drawRect(MID_X + 4, MID_Y + 4, MID_X - 8, H - MID_Y - 8, GxEPD_BLACK);

    display.setFont(&FreeMonoBold9pt7b);
    printCentered(MID_X / 2, 155, "Wrong");
    printCentered(MID_X + MID_X / 2, 155, "Correct");
  } while (display.nextPage());
}

static void pickNextCard() {
  currentIndex = random(0, NUM_PAIRS);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("004_esp_s3_touch_flashcard");

  pinMode(PIN_EPD_PWR, OUTPUT);
  digitalWrite(PIN_EPD_PWR, LOW);   // rail ON (active low)

  pinMode(PIN_TP_INT, INPUT_PULLUP);
  ft6336Reset();
  Wire.begin(PIN_TP_SDA, PIN_TP_SCL, 400000);

  delay(100);
  SPI.begin(PIN_EPD_SCK, -1, PIN_EPD_MOSI, PIN_EPD_CS);
  display.init(0, true, 2, false);
  display.setRotation(0);
  display.setTextColor(GxEPD_BLACK);

  randomSeed(esp_random());
  pickNextCard();
  gameState = STATE_PROMPT;
  drawPromptScreen();

  Serial.println("Touch lower half = Reveal; after reveal, LL = Wrong, LR = Correct");
}

void loop() {
  static bool wasTouching = false;

  int x, y;
  bool touching = readTouch(x, y);

  if (touching && !wasTouching) {
    Zone z = zoneOf(x, y);
    Serial.printf("touch-down (%3d,%3d) zone=%d state=%d\n", x, y, (int)z, (int)gameState);

    if (gameState == STATE_PROMPT) {
      if (z == ZONE_LL || z == ZONE_LR) {
        gameState = STATE_ANSWER;
        drawAnswerScreen();
      }
    } else { // STATE_ANSWER
      if (z == ZONE_LL || z == ZONE_LR) {
        Serial.println(z == ZONE_LL ? "-> Wrong" : "-> Correct");
        pickNextCard();
        gameState = STATE_PROMPT;
        drawPromptScreen();
      }
    }
  }
  wasTouching = touching;

  delay(30);   // ~33 Hz poll; also debounces
}
