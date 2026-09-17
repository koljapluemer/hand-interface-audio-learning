/*
 * 007_esp_s3_touch_flashcard_ble
 *
 * Board: Waveshare ESP32-S3-ePaper-1.54
 *        (200x200 black/white e-paper, FT6336 capacitive touch, I2C 0x38,
 *         integrated microSD/TF slot on the ESP32-S3 SDMMC peripheral)
 *
 * Same {front, back} flashcard game and on-device SD storage as 006, but the
 * sync flow no longer opens a Wi-Fi access point. Instead the device exposes
 * a Nordic UART Service (NUS) over BLE, and the companion "sync.html" page in
 * this folder (Web Bluetooth, Chrome/Edge desktop or Android -- NOT iOS
 * Safari, which has no Web Bluetooth support) connects to it directly:
 *
 *   - Prompt/Answer/Settings screens: identical to 006.
 *   - Settings -> "Sync mode": starts BLE advertising and shows the device
 *     name to pick in the companion page's "Connect" dialog, plus a live
 *     "Waiting for connection... / Connected!" line. No SSID/password/URL to
 *     type anywhere -- the phone or laptop's own Bluetooth stack does the
 *     pairing dance.
 *   - The game loop is fully paused while sync mode is up, same as 006.
 *
 * BLE contract (must match sync.html):
 *   Device name : "Flashcards"
 *   Service     : 6E400001-B5A3-F393-E0A9-E50E24DCCA9E   (Nordic UART Service)
 *   RX (write)  : 6E400002-B5A3-F393-E0A9-E50E24DCCA9E   app -> device
 *   TX (notify) : 6E400003-B5A3-F393-E0A9-E50E24DCCA9E   device -> app
 *
 * The RX/TX pair carries a tiny framed protocol instead of raw HTTP, since a
 * single BLE attribute value is capped at 512 bytes and the negotiated MTU
 * can be much smaller than the whole cards.tsv:
 *
 *   GET (app -> device): single byte 0x01.
 *   GET response (device -> app): one notify with [0x01, len:u32 LE], then
 *     as many notifies as needed carrying raw cards.tsv bytes, chunked at
 *     BLE_CHUNK bytes, until `len` bytes have been sent.
 *   PUT (app -> device): one write with [0x02, len:u32 LE], then as many
 *     writes as needed carrying raw body bytes (chunked at BLE_CHUNK), until
 *     `len` bytes have been sent. Device overwrites /cards.tsv with the
 *     reassembled body.
 *   PUT response (device -> app): one notify with [0x02, status], status 0
 *     = saved OK, 1 = error (no SD / write failed).
 *
 *   STAT GET (app -> device): single byte 0x03. Read-only debug/interest
 *     stats for the sync page: sends /state.tsv (front\tback\tbox\tpracticed
 *     per line, one per card) -- see the scheduler section below.
 *   STAT GET response (device -> app): one notify with [0x03, len:u32 LE],
 *     then chunks of raw state.tsv bytes, same framing as the cards GET.
 *
 * setup() calls BLEDevice::setMTU(247) so a fresh connection negotiates a
 * payload well above BLE_CHUNK; both sides assume that headroom rather than
 * probing the actual MTU.
 *
 * Storage split, touch, power-latch and text rendering are unchanged from
 * 006 -- see that file's header for the pin/bus rationale.
 *
 * Libraries: GxEPD2, U8g2_for_Adafruit_GFX, and the ESP32 Arduino core's
 * bundled BLE library (BLEDevice/BLEServer/BLE2902 -- no extra install,
 * same one used by 004_esp_c3_ble_buttons) plus SD_MMC. Needs core >= 2.0.7
 * for SD_MMC.setPins(). Arduino IDE settings: same as
 * 003_esp_s3_touch_quadrants_on_serial.
 */

#include <Wire.h>
#include <SPI.h>
#include <math.h>
#include <vector>
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "SD_MMC.h"

// Big font for the front row / back row; small font for button labels and the
// settings/sync text. Both "_tf" fonts cover ASCII + Latin-1 Supplement.
static const uint8_t *FONT_BIG   = u8g2_font_9x15_tf;
static const uint8_t *FONT_SMALL = u8g2_font_6x12_tf;

// ---- Pin map (Waveshare user_config.h + 04_SD_Card/sdcard_bsp.cpp) ----
static const int PIN_TP_SDA  = 47;
static const int PIN_TP_SCL  = 48;
static const int PIN_TP_INT  = 21;   // active low; unused (we poll)
static const int PIN_TP_RST  = 7;
static const int PIN_EPD_PWR = 6;    // e-paper power rail, active LOW

static const int PIN_VBAT_PWR = 17;  // battery self-latch (see 005 notes)
static const int PIN_PWR_BTN  = 18;  // active LOW
static const uint32_t PWR_LONGPRESS_MS = 1000;

static const int PIN_EPD_CS   = 11;
static const int PIN_EPD_DC   = 10;
static const int PIN_EPD_RST  = 9;
static const int PIN_EPD_BUSY = 8;
static const int PIN_EPD_SCK  = 12;
static const int PIN_EPD_MOSI = 13;

// SD card on the SDMMC peripheral, 1-bit mode.
static const int PIN_SD_CLK = 39;
static const int PIN_SD_CMD = 41;
static const int PIN_SD_D0  = 40;

GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> display(
    GxEPD2_154_D67(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));

U8G2_FOR_ADAFRUIT_GFX u8f;

static const uint8_t FT6336_ADDR = 0x38;

// ---- Panel geometry ----
static const int W = 200, H = 200;
static const int MID_X = W / 2;

// Card screen (matches the 005/006 layout).
static const int ROW_WORD_Y = 32;   // big font, front
static const int DASH_Y     = 52;
static const int ROW_MEAN_Y = 84;   // big font, back (answer state)
static const int DIV_Y      = 112;
static const int BTN_Y = DIV_Y + 4;
static const int BTN_H = H - DIV_Y - 8;
static const int WIDE_BTN_X = 4,          WIDE_BTN_W  = W - 8;        // Reveal
static const int LEFT_BTN_X = 4,          LEFT_BTN_W  = MID_X - 8;    // Wrong
static const int RIGHT_BTN_X = MID_X + 4, RIGHT_BTN_W = MID_X - 8;    // Correct
static const int WIDE_CX  = WIDE_BTN_X + WIDE_BTN_W / 2;
static const int LEFT_CX  = LEFT_BTN_X + LEFT_BTN_W / 2;
static const int RIGHT_CX = RIGHT_BTN_X + RIGHT_BTN_W / 2;
static const int BTN_LABEL_Y = 163;

// Settings gear: drawn tiny in the top-right corner; the tap target is the
// ~1 cm (64 px) corner square, which is dead space in the prompt state
// (Reveal lives below DIV_Y, and the front row is never itself a tap target).
// A very wide word can render pixels under this square, so a deliberate tap
// straight on those letters would open Settings -- acceptable trade.
static const int GEAR_CX = 184, GEAR_CY = 13;
static const int GEAR_TOUCH_X = W - 64;   // 136
static const int GEAR_TOUCH_H = 64;

// Settings screen buttons.
static const int SET_SYNC_X = 16, SET_SYNC_Y = 52,  SET_SYNC_W = 168, SET_SYNC_H = 58;
static const int SET_BACK_X = 16, SET_BACK_Y = 126, SET_BACK_W = 168, SET_BACK_H = 58;

// Sync screen "Restart" button and live status line.
static const int SYNC_RST_X = 16, SYNC_RST_Y = 160, SYNC_RST_W = 168, SYNC_RST_H = 32;
static const int SYNC_NAME_Y   = 86;
static const int SYNC_STATUS_Y = 112;
static const int SYNC_STATUS_RECT_Y = SYNC_STATUS_Y - 12, SYNC_STATUS_RECT_H = 18;

// ---- Orientation calibration (validated in 003) ----
static const bool SWAP_XY = false;
static const bool FLIP_X  = false;
static const bool FLIP_Y  = false;

// ---- Screens / state ----
enum Screen { SCREEN_CARD, SCREEN_SETTINGS, SCREEN_SYNC };
enum CardPhase { PHASE_PROMPT, PHASE_ANSWER };
static Screen    screen = SCREEN_CARD;
static CardPhase phase  = PHASE_PROMPT;

// ---- Flashcards (loaded from SD) ----
struct Flashcard { String front, back; int box; bool practiced; };
static std::vector<Flashcard> cards;
static int  currentIndex = 0;
static bool sdOk = false;

// ---- Scheduler: session queue over `cards`, by index into that vector ----
static const int NEW_CARDS_PER_SESSION = 12;
static const int BUBBLE_PASSES         = 2;
static const int INCORRECT_DELAY_MIN   = 4;   // reappear in 4..7 trials
static const int INCORRECT_DELAY_MAX   = 7;

static std::vector<int> queue;     // play order for this session
static size_t queuePos = 0;        // index into `queue` of the card on screen

// ---- BLE (Nordic UART Service) ----
#define DEVICE_NAME      "Flashcards"
#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

static const size_t BLE_CHUNK = 180;   // payload bytes per notify/write; well
                                        // under the MTU(247)-3 headroom below

static BLEServer *pServer = nullptr;
static BLECharacteristic *pTxCharacteristic = nullptr;
static volatile bool bleConnected = false;

enum RxState { RX_IDLE, RX_BODY };
static RxState rxState = RX_IDLE;
static std::vector<uint8_t> rxBuffer;
static size_t rxExpected = 0;
static size_t rxReceived = 0;

static inline size_t minSize(size_t a, size_t b) { return a < b ? a : b; }

// ---- Touch (FT6336) ----
static void ft6336Reset() {
  pinMode(PIN_TP_RST, OUTPUT);
  digitalWrite(PIN_TP_RST, HIGH); delay(20);
  digitalWrite(PIN_TP_RST, LOW);  delay(20);
  digitalWrite(PIN_TP_RST, HIGH); delay(150);
}

static bool ft6336Read(uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(FT6336_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
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

// One "true" per fresh finger-down, with the touch-down point. The single
// static is fine: loop() and syncLoop() never run at the same time.
static bool pollTapDown(int &x, int &y) {
  static bool was = false;
  bool now = readTouch(x, y);
  bool down = now && !was;
  was = now;
  return down;
}

static bool inRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

// ---- Text helpers (same pattern as 005/006) ----
static void selectFont(const uint8_t *font) {
  u8f.setFont(font);
  u8f.setFontMode(1);
}

static void printCentered(int cx, int baselineY, const char *text) {
  int w = u8f.getUTF8Width(text);
  u8f.setCursor(cx - w / 2, baselineY);
  u8f.print(text);
}

static void drawDashedLine(int y) {
  for (int x = 0; x < W; x += 8) {
    display.drawLine(x, y, min(x + 4, W - 1), y, GxEPD_BLACK);
  }
}

// ~14 px gear glyph centered on (cx, cy): eight spokes plus a hub ring.
static void drawGear(int cx, int cy) {
  for (int i = 0; i < 8; i++) {
    float a = i * (float)PI / 4.0f;
    int dx0 = (int)lroundf(3 * cosf(a)), dy0 = (int)lroundf(3 * sinf(a));
    int dx1 = (int)lroundf(7 * cosf(a)), dy1 = (int)lroundf(7 * sinf(a));
    display.drawLine(cx + dx0, cy + dy0, cx + dx1, cy + dy1, GxEPD_BLACK);
  }
  display.fillCircle(cx, cy, 4, GxEPD_WHITE);
  display.drawCircle(cx, cy, 4, GxEPD_BLACK);
  display.fillCircle(cx, cy, 1, GxEPD_BLACK);
}

// Instant touch-down acknowledgment: inverts a box via a fast partial refresh
// before the slower real transition. `label` may be nullptr (e.g. the gear).
static void flashBox(int x, int y, int w, int h, const char *label, const uint8_t *font) {
  display.setPartialWindow(x, y, w, h);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_BLACK);
    if (label) {
      selectFont(font);
      u8f.setForegroundColor(GxEPD_WHITE);
      printCentered(x + w / 2, y + h / 2 + 5, label);
    }
  } while (display.nextPage());
  u8f.setForegroundColor(GxEPD_BLACK);
}

// ---- Screen drawing ----
static void drawCardPrompt() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    if (cards.empty()) {
      selectFont(FONT_SMALL);
      printCentered(MID_X, 74, "No flashcards on SD.");
      printCentered(MID_X, 94, "Tap the gear, then");
      printCentered(MID_X, 110, "\"Sync mode\", to add some.");
    } else {
      selectFont(FONT_BIG);
      u8f.setCursor(6, ROW_WORD_Y);
      u8f.print(cards[currentIndex].front);

      display.drawLine(0, DIV_Y, W - 1, DIV_Y, GxEPD_BLACK);
      display.drawRect(WIDE_BTN_X, BTN_Y, WIDE_BTN_W, BTN_H, GxEPD_BLACK);
      selectFont(FONT_BIG);
      printCentered(WIDE_CX, BTN_LABEL_Y, "Reveal");
    }

    drawGear(GEAR_CX, GEAR_CY);
  } while (display.nextPage());
}

static void drawCardAnswer() {
  display.setPartialWindow(0, 0, W, H);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    selectFont(FONT_BIG);
    u8f.setCursor(6, ROW_WORD_Y);
    u8f.print(cards[currentIndex].front);

    drawDashedLine(DASH_Y);

    selectFont(FONT_BIG);
    u8f.setCursor(6, ROW_MEAN_Y);
    u8f.print(cards[currentIndex].back);

    display.drawLine(0, DIV_Y, W - 1, DIV_Y, GxEPD_BLACK);
    display.drawLine(MID_X, DIV_Y, MID_X, H - 1, GxEPD_BLACK);
    display.drawRect(LEFT_BTN_X, BTN_Y, LEFT_BTN_W, BTN_H, GxEPD_BLACK);
    display.drawRect(RIGHT_BTN_X, BTN_Y, RIGHT_BTN_W, BTN_H, GxEPD_BLACK);

    selectFont(FONT_SMALL);
    printCentered(LEFT_CX, BTN_LABEL_Y, "Wrong");
    printCentered(RIGHT_CX, BTN_LABEL_Y, "Correct");
  } while (display.nextPage());
}

static void drawSettings() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    selectFont(FONT_BIG);
    printCentered(MID_X, 28, "Settings");

    display.drawRect(SET_SYNC_X, SET_SYNC_Y, SET_SYNC_W, SET_SYNC_H, GxEPD_BLACK);
    printCentered(SET_SYNC_X + SET_SYNC_W / 2, SET_SYNC_Y + SET_SYNC_H / 2 + 5, "Sync mode");

    display.drawRect(SET_BACK_X, SET_BACK_Y, SET_BACK_W, SET_BACK_H, GxEPD_BLACK);
    printCentered(SET_BACK_X + SET_BACK_W / 2, SET_BACK_Y + SET_BACK_H / 2 + 5, "Back");
  } while (display.nextPage());
}

static void drawSyncScreen() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    selectFont(FONT_BIG);
    printCentered(MID_X, 22, "Sync mode");

    selectFont(FONT_SMALL);
    printCentered(MID_X, 46, "1. Open the sync page");
    printCentered(MID_X, 62, "2. Tap Connect, pick:");

    selectFont(FONT_BIG);
    printCentered(MID_X, SYNC_NAME_Y, DEVICE_NAME);

    selectFont(FONT_SMALL);
    // Status line at SYNC_STATUS_Y is filled in by drawSyncStatus() below.
    printCentered(MID_X, 134, "3. Edit + Save, then:");

    display.drawRect(SYNC_RST_X, SYNC_RST_Y, SYNC_RST_W, SYNC_RST_H, GxEPD_BLACK);
    selectFont(FONT_BIG);
    printCentered(SYNC_RST_X + SYNC_RST_W / 2, SYNC_RST_Y + SYNC_RST_H / 2 + 5, "Restart");
  } while (display.nextPage());
}

// Partial refresh of just the status line, so connect/disconnect events don't
// redraw (and flash) the whole sync screen.
static void drawSyncStatus(const char *text) {
  display.setPartialWindow(0, SYNC_STATUS_RECT_Y, W, SYNC_STATUS_RECT_H);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    selectFont(FONT_SMALL);
    printCentered(MID_X, SYNC_STATUS_Y, text);
  } while (display.nextPage());
}

static void drawFatalSd() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    selectFont(FONT_BIG);
    printCentered(MID_X, 90, "No SD card");
    selectFont(FONT_SMALL);
    printCentered(MID_X, 116, "Insert a FAT32 card,");
    printCentered(MID_X, 132, "then reset.");
  } while (display.nextPage());
}

// ---- Card data ----
static bool loadCards() {
  cards.clear();
  File f = SD_MMC.open("/cards.tsv", FILE_READ);
  if (!f) { Serial.println("no /cards.tsv yet"); return false; }
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.replace("\r", "");
    if (line.isEmpty()) continue;
    int t = line.indexOf('\t');
    if (t < 0) continue;
    Flashcard c;
    c.front = line.substring(0, t);  c.front.trim();
    c.back  = line.substring(t + 1); c.back.trim();
    if (c.front.isEmpty() && c.back.isEmpty()) continue;
    c.box = 0;
    c.practiced = false;
    cards.push_back(c);
  }
  f.close();
  Serial.printf("loaded %u card(s)\n", (unsigned)cards.size());
  return !cards.empty();
}

// ---- Scheduler state (box + practiced flag per card) ----
// Kept in its own file, separate from cards.tsv, so the BLE sync page --
// which only ever reads/writes plain front\tback rows -- can never see or
// clobber it. Reconciled against the current `cards` by front+back match,
// so edits/adds/removes made via sync just fall out of the match on the
// next boot instead of corrupting anything.
static void loadState() {
  File f = SD_MMC.open("/state.tsv", FILE_READ);
  if (!f) { Serial.println("no /state.tsv yet (fresh deck)"); return; }

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.replace("\r", "");
    if (line.isEmpty()) continue;

    // Parse from the right (...front\tback\tbox\tpracticed) so an
    // embedded tab in front/back, however unlikely, can't desync the count.
    int lastTab = line.lastIndexOf('\t');
    if (lastTab < 0) continue;
    int boxTab = line.lastIndexOf('\t', lastTab - 1);
    if (boxTab < 0) continue;
    String practicedStr = line.substring(lastTab + 1);
    String boxStr       = line.substring(boxTab + 1, lastTab);
    String frontBack     = line.substring(0, boxTab);

    int fbTab = frontBack.indexOf('\t');
    if (fbTab < 0) continue;
    String front = frontBack.substring(0, fbTab);      front.trim();
    String back  = frontBack.substring(fbTab + 1);     back.trim();

    for (auto &c : cards) {
      if (c.front == front && c.back == back) {
        c.box = boxStr.toInt();
        c.practiced = practicedStr.toInt() != 0;
        break;
      }
    }
  }
  f.close();
}

static void saveState() {
  if (!sdOk) return;
  File f = SD_MMC.open("/state.tsv", FILE_WRITE);   // "w" -> truncates
  if (!f) { Serial.println("state save FAILED (no SD?)"); return; }
  for (auto &c : cards) {
    f.print(c.front); f.print('\t');
    f.print(c.back);  f.print('\t');
    f.print(c.box);   f.print('\t');
    f.println(c.practiced ? 1 : 0);
  }
  f.close();
}

// ---- Session queue: initial order on boot ----
static void shuffleQueue(std::vector<int> &q) {
  for (int i = (int)q.size() - 1; i > 0; i--) {
    int j = random(0, i + 1);
    int t = q[i]; q[i] = q[j]; q[j] = t;
  }
}

// Fully random shuffle, then an intentionally incomplete (2-pass) bubble
// sort by box, so lower-box items tend to end up earlier on average -- a
// gentle bias, not a real sort. Never-practiced cards are skipped by every
// comparison they're part of, so they stay wherever the shuffle put them;
// box changes made during the session only affect ordering starting next
// boot, never mid-session.
static void buildSessionQueue() {
  queue.clear();
  queuePos = 0;
  if (cards.empty()) return;

  // Only the next NEW_CARDS_PER_SESSION never-practiced cards (in deck
  // order) join the queue this session; the rest sit out entirely until a
  // future boot introduces them.
  std::vector<int> newIdx;
  for (int i = 0; i < (int)cards.size() && (int)newIdx.size() < NEW_CARDS_PER_SESSION; i++) {
    if (!cards[i].practiced) newIdx.push_back(i);
  }
  for (int i = 0; i < (int)cards.size(); i++) {
    if (cards[i].practiced) queue.push_back(i);
  }
  for (int idx : newIdx) queue.push_back(idx);

  shuffleQueue(queue);

  for (int pass = 0; pass < BUBBLE_PASSES; pass++) {
    for (size_t i = 0; i + 1 < queue.size(); i++) {
      int a = queue[i], b = queue[i + 1];
      if (!cards[a].practiced || !cards[b].practiced) continue;   // new cards stay put
      if (cards[a].box > cards[b].box) { queue[i] = b; queue[i + 1] = a; }
    }
  }

  currentIndex = queue[queuePos];
}

// ---- In-session grading ----
// Box/practiced persist to SD immediately; list position is session-only
// (a fresh shuffle+sort happens every boot, so there's nothing to save).
static void gradeCorrect() {
  if (queue.empty()) return;
  int idx = queue[queuePos];
  cards[idx].box++;
  cards[idx].practiced = true;
  saveState();

  queue.erase(queue.begin() + queuePos);
  // After removing the graded card, whatever slid into queuePos (if
  // anything) is the next card to show; if it was the last card, wrap.
  size_t nextPos = (queuePos < queue.size()) ? queuePos : 0;
  queue.push_back(idx);   // appending never shifts indices <= nextPos
  queuePos = nextPos;
  currentIndex = queue[queuePos];
}

static void gradeIncorrect() {
  if (queue.empty()) return;
  int idx = queue[queuePos];
  cards[idx].box = max(0, cards[idx].box - 2);
  cards[idx].practiced = true;
  saveState();

  queue.erase(queue.begin() + queuePos);
  size_t nextPos = (queuePos < queue.size()) ? queuePos : 0;
  int delay = random(INCORRECT_DELAY_MIN, INCORRECT_DELAY_MAX + 1);
  size_t insertPos = minSize(nextPos + (size_t)delay, queue.size());
  queue.insert(queue.begin() + insertPos, idx);   // insertPos >= nextPos, so
                                                   // the element at nextPos
                                                   // is never shifted by this
  queuePos = nextPos;
  currentIndex = queue[queuePos];
}

// ---- Power button (works on the game loop and the sync loop) ----
static void powerOff() {
  Serial.println("PWR long-press -> shutting down");
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    selectFont(FONT_BIG);
    printCentered(MID_X, 100, "Bye!");
  } while (display.nextPage());
  digitalWrite(PIN_VBAT_PWR, LOW);
  while (true) delay(1000);
}

static void handlePowerButton() {
  static bool down = false;
  static uint32_t downAt = 0;
  if (digitalRead(PIN_PWR_BTN) == LOW) {
    if (!down) { down = true; downAt = millis(); }
    else if (millis() - downAt >= PWR_LONGPRESS_MS) powerOff();
  } else {
    down = false;
  }
}

// ---- BLE protocol: GET (send /cards.tsv), GET state (send /state.tsv,
// read-only, for the sync page's stats panel), and PUT (overwrite cards) ----
static void sendFileOverBle(const char *path, uint8_t cmd) {
  String content;
  if (sdOk) {
    File f = SD_MMC.open(path, FILE_READ);
    if (f) { content = f.readString(); f.close(); }
  }
  size_t total = content.length();
  const uint8_t *data = (const uint8_t *)content.c_str();

  uint8_t header[5];
  header[0] = cmd;
  header[1] = (uint8_t)(total & 0xFF);
  header[2] = (uint8_t)((total >> 8) & 0xFF);
  header[3] = (uint8_t)((total >> 16) & 0xFF);
  header[4] = (uint8_t)((total >> 24) & 0xFF);
  pTxCharacteristic->setValue(header, sizeof(header));
  pTxCharacteristic->notify();

  size_t sent = 0;
  while (sent < total) {
    size_t n = minSize(BLE_CHUNK, total - sent);
    pTxCharacteristic->setValue(data + sent, n);
    pTxCharacteristic->notify();
    sent += n;
    delay(15);   // let the BLE stack drain the notify queue between packets
  }
  Serial.printf("BLE: sent %u bytes (%s)\n", (unsigned)total, path);
}

static void sendCardsOverBle() { sendFileOverBle("/cards.tsv", 0x01); }
static void sendStateOverBle() { sendFileOverBle("/state.tsv", 0x03); }

static void finishPut() {
  bool ok = sdOk;
  if (ok) {
    File f = SD_MMC.open("/cards.tsv", FILE_WRITE);   // "w" -> truncates
    if (!f) {
      ok = false;
    } else {
      if (!rxBuffer.empty()) f.write(rxBuffer.data(), rxBuffer.size());
      f.close();
    }
  }
  Serial.printf("BLE: wrote /cards.tsv (%u bytes) -> %s\n",
                (unsigned)rxBuffer.size(), ok ? "ok" : "FAILED");

  uint8_t resp[2] = { 0x02, (uint8_t)(ok ? 0x00 : 0x01) };
  pTxCharacteristic->setValue(resp, sizeof(resp));
  pTxCharacteristic->notify();

  rxState = RX_IDLE;
  rxBuffer.clear();
  rxBuffer.shrink_to_fit();
}

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    uint8_t *data = pCharacteristic->getData();
    size_t len = pCharacteristic->getLength();
    if (len == 0) return;

    if (rxState == RX_IDLE) {
      if (data[0] == 0x01) {                       // GET
        sendCardsOverBle();
      } else if (data[0] == 0x03) {                 // GET state (stats)
        sendStateOverBle();
      } else if (data[0] == 0x02 && len >= 5) {     // PUT header
        uint32_t total = (uint32_t)data[1] | ((uint32_t)data[2] << 8) |
                          ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
        rxExpected = total;
        rxReceived = 0;
        rxBuffer.assign(total, 0);
        if (total == 0) finishPut();
        else rxState = RX_BODY;
      }
    } else {   // RX_BODY: raw body bytes, no framing
      size_t n = minSize(len, rxExpected - rxReceived);
      memcpy(rxBuffer.data() + rxReceived, data, n);
      rxReceived += n;
      if (rxReceived >= rxExpected) finishPut();
    }
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) override {
    bleConnected = true;
    rxState = RX_IDLE;
    rxBuffer.clear();
    rxBuffer.shrink_to_fit();
    Serial.println("BLE: central connected");
  }
  void onDisconnect(BLEServer *pServer) override {
    bleConnected = false;
    Serial.println("BLE: central disconnected, re-advertising");
    pServer->startAdvertising();
  }
};

// ---- Sync mode: game paused, BLE up until "Restart" ----
static void syncLoop() {
  bool lastConnected = false;
  drawSyncStatus("Waiting for connection...");

  for (;;) {
    handlePowerButton();

    if (bleConnected != lastConnected) {
      lastConnected = bleConnected;
      drawSyncStatus(bleConnected ? "Connected!" : "Waiting for connection...");
    }

    int x, y;
    if (pollTapDown(x, y) && inRect(x, y, SYNC_RST_X, SYNC_RST_Y, SYNC_RST_W, SYNC_RST_H)) {
      flashBox(SYNC_RST_X, SYNC_RST_Y, SYNC_RST_W, SYNC_RST_H, "Restart", FONT_BIG);
      delay(300);
      ESP.restart();
    }
    delay(10);
  }
}

static void enterSyncMode() {
  screen = SCREEN_SYNC;
  Serial.println("entering BLE sync mode");

  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(247);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService *service = pServer->createService(NUS_SERVICE_UUID);

  pTxCharacteristic = service->createCharacteristic(NUS_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pTxCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic *rxChar = service->createCharacteristic(NUS_RX_UUID, BLECharacteristic::PROPERTY_WRITE);
  rxChar->setCallbacks(new RxCallbacks());

  service->start();

  BLEAdvertising *adv = pServer->getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->start();
  Serial.println("BLE advertising as \"" DEVICE_NAME "\"");

  drawSyncScreen();
  syncLoop();                         // never returns (exits via ESP.restart)
}

// ---- Setup / loop ----
void setup() {
  pinMode(PIN_VBAT_PWR, OUTPUT);
  digitalWrite(PIN_VBAT_PWR, HIGH);  // latch battery power first
  pinMode(PIN_PWR_BTN, INPUT_PULLUP);

  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("007_esp_s3_touch_flashcard_ble");

  pinMode(PIN_EPD_PWR, OUTPUT);
  digitalWrite(PIN_EPD_PWR, LOW);    // e-paper rail ON (active low)

  pinMode(PIN_TP_INT, INPUT_PULLUP);
  ft6336Reset();
  Wire.begin(PIN_TP_SDA, PIN_TP_SCL, 400000);

  delay(100);
  SPI.begin(PIN_EPD_SCK, -1, PIN_EPD_MOSI, PIN_EPD_CS);
  display.init(0, true, 2, false);
  display.setRotation(0);

  u8f.begin(display);
  u8f.setFontMode(1);
  u8f.setForegroundColor(GxEPD_BLACK);

  // SD on the dedicated SDMMC peripheral, 1-bit mode -- separate from the
  // e-paper SPI bus.
  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
  sdOk = SD_MMC.begin("/sdcard", true);
  Serial.printf("SD mount: %s\n", sdOk ? "ok" : "FAILED");

  if (!sdOk) {
    drawFatalSd();
    while (true) { handlePowerButton(); delay(50); }
  }

  loadCards();
  loadState();

  randomSeed(esp_random());
  buildSessionQueue();
  saveState();   // so /state.tsv (and the sync page's stats) are fresh even
                  // before the first grade of the session, e.g. right after
                  // a deck edit adds cards that were never graded before
  screen = SCREEN_CARD;
  phase  = PHASE_PROMPT;
  drawCardPrompt();

  Serial.println("gear (top-right) = Settings; lower half = Reveal / Wrong / Correct");
}

void loop() {
  handlePowerButton();

  int x, y;
  if (pollTapDown(x, y)) {
    Serial.printf("tap (%3d,%3d) screen=%d phase=%d\n", x, y, (int)screen, (int)phase);

    if (screen == SCREEN_CARD && phase == PHASE_PROMPT) {
      if (x >= GEAR_TOUCH_X && y < GEAR_TOUCH_H) {
        flashBox(GEAR_CX - 16, 0, 32, GEAR_CY + 16, nullptr, FONT_SMALL);
        screen = SCREEN_SETTINGS;
        drawSettings();
      } else if (y >= DIV_Y && !cards.empty()) {
        flashBox(WIDE_BTN_X, BTN_Y, WIDE_BTN_W, BTN_H, "Reveal", FONT_BIG);
        phase = PHASE_ANSWER;
        drawCardAnswer();
      }
    } else if (screen == SCREEN_CARD && phase == PHASE_ANSWER) {
      if (y >= DIV_Y && x < MID_X) {
        flashBox(LEFT_BTN_X, BTN_Y, LEFT_BTN_W, BTN_H, "Wrong", FONT_SMALL);
        Serial.println("-> Wrong");
        gradeIncorrect(); phase = PHASE_PROMPT; drawCardPrompt();
      } else if (y >= DIV_Y && x >= MID_X) {
        flashBox(RIGHT_BTN_X, BTN_Y, RIGHT_BTN_W, BTN_H, "Correct", FONT_SMALL);
        Serial.println("-> Correct");
        gradeCorrect(); phase = PHASE_PROMPT; drawCardPrompt();
      }
    } else if (screen == SCREEN_SETTINGS) {
      if (inRect(x, y, SET_SYNC_X, SET_SYNC_Y, SET_SYNC_W, SET_SYNC_H)) {
        flashBox(SET_SYNC_X, SET_SYNC_Y, SET_SYNC_W, SET_SYNC_H, "Sync mode", FONT_BIG);
        enterSyncMode();                // does not return
      } else if (inRect(x, y, SET_BACK_X, SET_BACK_Y, SET_BACK_W, SET_BACK_H)) {
        flashBox(SET_BACK_X, SET_BACK_Y, SET_BACK_W, SET_BACK_H, "Back", FONT_BIG);
        screen = SCREEN_CARD; phase = PHASE_PROMPT; drawCardPrompt();
      }
    }
  }

  delay(30);   // ~33 Hz poll; also debounces
}
