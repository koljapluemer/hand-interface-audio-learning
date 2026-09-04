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
 * Also latches the board's battery power on at boot and honors the physical
 * PWR button (long-press to shut down), so it works untethered from USB --
 * see the PIN_VBAT_PWR / PIN_PWR_BTN comments below.
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
static const int PIN_EPD_PWR = 6;    // e-paper power rail, active LOW

// Battery self-latch: pressing the physical PWR button gives the board a
// momentary jolt of power; firmware must drive PIN_VBAT_PWR HIGH within that
// window or it dies again. Holding PWR_BUTTON for PWR_LONGPRESS_MS drives it
// back LOW to cut power for a clean shutdown. Matches Waveshare's official
// board_power_bsp/button_bsp reference (waveshareteam/ESP32-S3-ePaper-1.54).
// On USB power this is a no-op for staying alive (USB supplies the board
// regardless), but we still latch/unlatch it so behavior matches when the
// board is later run untethered.
static const int PIN_VBAT_PWR = 17;
static const int PIN_PWR_BTN  = 18;  // active LOW (button ties it to GND)
static const uint32_t PWR_LONGPRESS_MS = 1000;

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

// Button boxes (shared by the drawing code and the touch-down flash, so
// they always line up exactly).
static const int BTN_Y   = MID_Y + 4;
static const int BTN_H   = H - MID_Y - 8;
static const int WIDE_BTN_X = 4,          WIDE_BTN_W  = W - 8;        // Reveal
static const int LEFT_BTN_X = 4,          LEFT_BTN_W  = MID_X - 8;    // Wrong
static const int RIGHT_BTN_X = MID_X + 4, RIGHT_BTN_W = MID_X - 8;    // Correct
static const int WIDE_CX  = WIDE_BTN_X + WIDE_BTN_W / 2;
static const int LEFT_CX  = LEFT_BTN_X + LEFT_BTN_W / 2;
static const int RIGHT_CX = RIGHT_BTN_X + RIGHT_BTN_W / 2;
static const int BTN_LABEL_Y = 155;

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

// Instant touch-down acknowledgment: inverts the tapped button (black fill,
// white label) via a small, fast partial-window refresh, before the real
// (slower) screen transition runs. Confirms the tap was registered well
// before the actual content change is visible.
static void flashBox(int x, int y, int w, int h, const char *label, const GFXfont *font) {
  display.setPartialWindow(x, y, w, h);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_BLACK);
    display.setFont(font);
    display.setTextColor(GxEPD_WHITE);
    printCentered(x + w / 2, BTN_LABEL_Y, label);
  } while (display.nextPage());
  display.setTextColor(GxEPD_BLACK);
}

// Full refresh: used when loading a new random card, since it's always
// preceded by a partial refresh (the reveal) and a full refresh here keeps
// ghosting from ever accumulating across more than one partial update.
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
    display.drawRect(WIDE_BTN_X, BTN_Y, WIDE_BTN_W, BTN_H, GxEPD_BLACK);

    display.setFont(&FreeMonoBold12pt7b);
    printCentered(WIDE_CX, BTN_LABEL_Y, "Reveal");
  } while (display.nextPage());
}

// Partial refresh: reveal is on the fast path users tap through quickly, so
// this trades a little ghosting risk for ~0.3s updates instead of ~1-2s.
// Safe here because drawPromptScreen() always runs right after and does a
// full refresh, so ghosting never has more than one partial update to build.
static void drawAnswerScreen() {
  display.setPartialWindow(0, 0, W, H);
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
    display.drawRect(LEFT_BTN_X, BTN_Y, LEFT_BTN_W, BTN_H, GxEPD_BLACK);
    display.drawRect(RIGHT_BTN_X, BTN_Y, RIGHT_BTN_W, BTN_H, GxEPD_BLACK);

    display.setFont(&FreeMonoBold9pt7b);
    printCentered(LEFT_CX, BTN_LABEL_Y, "Wrong");
    printCentered(RIGHT_CX, BTN_LABEL_Y, "Correct");
  } while (display.nextPage());
}

static void pickNextCard() {
  currentIndex = random(0, NUM_PAIRS);
}

// Long-press on PWR: show a shutdown screen, then release the battery latch.
// On battery this cuts power outright; over USB the board stays alive (USB
// feeds it independently of the latch), so we park in an idle loop instead
// of falling back into the game.
static void powerOff() {
  Serial.println("PWR long-press -> shutting down");
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeMonoBold12pt7b);
    printCentered(MID_X, 100, "Bye!");
  } while (display.nextPage());

  digitalWrite(PIN_VBAT_PWR, LOW);   // release latch -- powers off on battery

  while (true) delay(1000);          // still running (e.g. on USB): stay idle
}

void setup() {
  // Latch battery power on first, before anything else -- the window between
  // the PWR button's momentary jolt and this line is the only thing keeping
  // the board alive when running untethered.
  pinMode(PIN_VBAT_PWR, OUTPUT);
  digitalWrite(PIN_VBAT_PWR, HIGH);
  pinMode(PIN_PWR_BTN, INPUT_PULLUP);

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
  static bool pwrBtnDown = false;
  static uint32_t pwrDownAt = 0;

  if (digitalRead(PIN_PWR_BTN) == LOW) {
    if (!pwrBtnDown) { pwrBtnDown = true; pwrDownAt = millis(); }
    else if (millis() - pwrDownAt >= PWR_LONGPRESS_MS) {
      powerOff();   // does not return on battery
    }
  } else {
    pwrBtnDown = false;
  }

  int x, y;
  bool touching = readTouch(x, y);

  if (touching && !wasTouching) {
    Zone z = zoneOf(x, y);
    Serial.printf("touch-down (%3d,%3d) zone=%d state=%d\n", x, y, (int)z, (int)gameState);

    if (gameState == STATE_PROMPT) {
      if (z == ZONE_LL || z == ZONE_LR) {
        flashBox(WIDE_BTN_X, BTN_Y, WIDE_BTN_W, BTN_H, "Reveal", &FreeMonoBold12pt7b);
        gameState = STATE_ANSWER;
        drawAnswerScreen();
      }
    } else { // STATE_ANSWER
      if (z == ZONE_LL) {
        flashBox(LEFT_BTN_X, BTN_Y, LEFT_BTN_W, BTN_H, "Wrong", &FreeMonoBold9pt7b);
        Serial.println("-> Wrong");
        pickNextCard();
        gameState = STATE_PROMPT;
        drawPromptScreen();
      } else if (z == ZONE_LR) {
        flashBox(RIGHT_BTN_X, BTN_Y, RIGHT_BTN_W, BTN_H, "Correct", &FreeMonoBold9pt7b);
        Serial.println("-> Correct");
        pickNextCard();
        gameState = STATE_PROMPT;
        drawPromptScreen();
      }
    }
  }
  wasTouching = touching;

  delay(30);   // ~33 Hz poll; also debounces
}
