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
 * can be much smaller than the whole cards.bin:
 *
 *   GET (app -> device): single byte 0x01.
 *   GET response (device -> app): one notify with [0x01, len:u32 LE], then
 *     as many notifies as needed carrying raw cards.bin bytes, chunked at
 *     BLE_CHUNK bytes, until `len` bytes have been sent.
 *   PUT (app -> device): one write with [0x02, len:u32 LE], then as many
 *     writes as needed carrying raw body bytes (chunked at BLE_CHUNK), until
 *     `len` bytes have been sent. Device overwrites /cards.bin with the
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
 * The GET/PUT framing above is content-agnostic (it just moves the bytes of
 * a path), which is what lets /cards.bin be a binary format instead of text:
 *
 *   /cards.bin: [magic "FCB1"][u16 bmpW LE][u16 bmpH LE], then per card
 *     [u16 frontTextLen][frontText UTF-8][u16 backTextLen][backText UTF-8]
 *     [frontBitmap][backBitmap], each bitmap bmpW x bmpH, 1bpp, MSB-first,
 *     rows padded to a byte -- i.e. exactly what Adafruit_GFX::drawBitmap()
 *     wants. The front/back text is carried along only so /state.tsv's
 *     box/practiced matching (by front+back string) keeps working; the
 *     device never renders it -- sync.html renders both fields to canvas
 *     (so the browser's font/shaping stack, not u8g2, handles Arabic
 *     joining, Vietnamese diacritics, CJK, etc.) and ships pre-dithered
 *     pixels. bmpW/bmpH in the header must match BMP_W/BMP_H below; the
 *     device loads only per-card byte offsets into RAM and reads each
 *     bitmap from SD on demand when drawing, so deck size isn't RAM-bound.
 *
 * setup() calls BLEDevice::setMTU(247) so a fresh connection negotiates
 * enough headroom for BLE_CHUNK (244 bytes, MTU-3) in one notify/write; both
 * sides assume that headroom rather than probing the actual MTU. It also
 * requests a short (7.5-15ms) connection interval right after connect, and
 * the RX characteristic advertises write-without-response so sync.html can
 * stream PUT bodies without waiting for a per-chunk ATT ack -- see
 * ServerCallbacks::onConnect() and the RX characteristic properties below.
 *
 * Storage split, touch, power-latch and text rendering are unchanged from
 * 006 -- see that file's header for the pin/bus rationale. FONT_BIG/SMALL
 * now only draw fixed ASCII UI chrome (button labels, settings/sync text);
 * card front/back are pre-rendered bitmaps, see /cards.bin above.
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
#include <cstring>
#include <vector>
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "SD_MMC.h"
#include "esp_system.h"   // esp_reset_reason() -- see resetReasonStr() below

// UI chrome only (button labels, settings/sync text, "No flashcards"
// message) -- all fixed ASCII strings. Card front/back are pre-rendered
// bitmaps from sync.html, see the /cards.bin note in the header comment.
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

// Card content: pre-rendered 1bpp bitmaps from sync.html (see /cards.bin in
// the header comment), blitted at fixed positions instead of drawn as text.
static const int BMP_W = 190, BMP_H = 36;
static const int BMP_ROW_BYTES = (BMP_W + 7) / 8;
static const size_t BMP_BYTES = (size_t)BMP_ROW_BYTES * BMP_H;
static const int FRONT_BMP_X = 5, FRONT_BMP_Y = 4;    // front row (prompt + answer)
static const int BACK_BMP_X  = 5, BACK_BMP_Y  = 58;   // back/translation row (answer only)

// Card screen (matches the 005/006 layout).
static const int DASH_Y     = 52;
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
// front/back text is kept only for /state.tsv matching, never rendered; the
// bitmaps themselves stay on SD and are read on demand by their byte offset
// into /cards.bin so RAM use doesn't grow with deck size.
struct Flashcard {
  String front, back;
  int box; bool practiced;
  uint32_t frontBmpOffset, backBmpOffset;
};
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

// NOTE: a device crash right after connect turned out to be caused by
// sending (notify() in a retry loop) directly from within RxCallbacks::
// onWrite() -- see PendingBleAction below for the real fix and explanation.
// It was NOT this chunk size (244, the full MTU(247)-3 headroom, was tried
// and still crashed; so did reverting to 180). Kept at 180, matching the
// previously-stable value, since there's no evidence larger helps and no
// reason to change two things at once right after finding the real cause.
static const size_t BLE_CHUNK = 180;   // payload bytes per notify/write

static BLEServer *pServer = nullptr;
static BLECharacteristic *pTxCharacteristic = nullptr;
static volatile bool bleConnected = false;
// Set by TxCallbacks::onStatus() whenever an indicate() call fails to queue
// (stack congested / out of send buffers) or times out waiting for the
// peer's confirmation -- sendChunkReliably() polls this to back off and
// retry only when actually needed, instead of a fixed per-chunk delay that
// would throttle every send whether or not it was necessary.
static volatile bool notifyCongested = false;

// PUT bodies (and GET responses, see sendFileOverBle() below) are streamed
// straight to/from the SD card in BLE_CHUNK-sized pieces rather than
// buffered whole in RAM -- a 1000-card deck is ~1.7MB, and round-tripping
// that through a std::vector on top of everything else this device already
// holds is exactly the kind of heap pressure that caused a crash right
// after a large save/load completed (confirmed: SD write and BLE ack both
// logged success, then a reboot). Streaming caps the RAM cost at one
// BLE_CHUNK-sized buffer regardless of deck size.
enum RxState { RX_IDLE, RX_BODY };
static RxState rxState = RX_IDLE;
static size_t rxExpected = 0;
static size_t rxReceived = 0;
static File putFile;         // open across the PUT's onWrite() calls
static bool putOk = false;   // false if sdOk was false, open failed, or a write came up short

// RxCallbacks::onWrite() runs on the NimBLE host task, inside that task's
// own (comparatively small) stack frame for the callback. Calling back into
// the BLE stack from there -- e.g. a loop of retried notify() calls to send
// a whole file -- is a documented cause of stack overflow crashes on this
// stack (confirmed here: reliably crashed right after connect, regardless
// of chunk size or connection params, with no visible backtrace because a
// stack overflow corrupts the very stack the panic handler would unwind).
// So onWrite() only ever sets one of these flags; the actual send/finish
// work happens in syncLoop() on the main Arduino task instead, which has a
// normal-sized stack and isn't nested inside a BLE callback at all.
enum PendingBleAction { PENDING_NONE, PENDING_SEND_CARDS, PENDING_SEND_STATE, PENDING_FINISH_PUT };
static volatile PendingBleAction pendingAction = PENDING_NONE;

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

// Reads one card's bitmap from /cards.bin into `bmpBuf` and blits it. No-op
// (leaves the area blank) on any SD hiccup rather than drawing garbage.
// Defined here, ahead of loadCards()/the SD-backed Flashcard struct below,
// only to sit next to the screen-drawing functions that call it.
static uint8_t bmpBuf[BMP_BYTES];
static void drawCardBitmap(uint32_t offset, int x, int y) {
  File f = SD_MMC.open("/cards.bin", FILE_READ);
  if (!f) return;
  if (f.seek(offset) && f.read(bmpBuf, BMP_BYTES) == BMP_BYTES) {
    display.drawBitmap(x, y, bmpBuf, BMP_W, BMP_H, GxEPD_BLACK);
  }
  f.close();
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
      drawCardBitmap(cards[currentIndex].frontBmpOffset, FRONT_BMP_X, FRONT_BMP_Y);

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

    drawCardBitmap(cards[currentIndex].frontBmpOffset, FRONT_BMP_X, FRONT_BMP_Y);
    drawDashedLine(DASH_Y);
    drawCardBitmap(cards[currentIndex].backBmpOffset, BACK_BMP_X, BACK_BMP_Y);

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
// Reads a u16-LE length prefix followed by that many UTF-8 bytes into a
// null-terminated buffer, returns false on short read (truncated/corrupt file).
static bool readLenPrefixedString(File &f, String &out) {
  uint8_t lenBuf[2];
  if (f.read(lenBuf, 2) != 2) return false;
  uint16_t len = lenBuf[0] | ((uint16_t)lenBuf[1] << 8);
  std::vector<char> buf(len + 1);
  if (len > 0 && f.read((uint8_t *)buf.data(), len) != len) return false;
  buf[len] = '\0';
  out = String(buf.data());
  return true;
}

static bool loadCards() {
  cards.clear();
  File f = SD_MMC.open("/cards.bin", FILE_READ);
  if (!f) { Serial.println("no /cards.bin yet"); return false; }

  uint8_t hdr[8];
  if (f.read(hdr, 8) != 8 || memcmp(hdr, "FCB1", 4) != 0) {
    Serial.println("cards.bin: missing/bad header");
    f.close();
    return false;
  }
  uint16_t bmpW = hdr[4] | ((uint16_t)hdr[5] << 8);
  uint16_t bmpH = hdr[6] | ((uint16_t)hdr[7] << 8);
  if (bmpW != BMP_W || bmpH != BMP_H) {
    Serial.printf("cards.bin: bitmap size %ux%u != expected %dx%d, ignoring\n",
                  bmpW, bmpH, BMP_W, BMP_H);
    f.close();
    return false;
  }

  while (f.available()) {
    Flashcard c;
    if (!readLenPrefixedString(f, c.front)) break;
    if (!readLenPrefixedString(f, c.back)) break;

    c.frontBmpOffset = f.position();
    c.backBmpOffset  = c.frontBmpOffset + BMP_BYTES;
    if (!f.seek(c.backBmpOffset + BMP_BYTES)) break;   // truncated record

    c.box = 0;
    c.practiced = false;
    cards.push_back(c);
  }
  f.close();
  Serial.printf("loaded %u card(s)\n", (unsigned)cards.size());
  return !cards.empty();
}

// ---- Scheduler state (box + practiced flag per card) ----
// Kept in its own file, separate from cards.bin, so the BLE sync page --
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

// ---- BLE protocol: GET (send /cards.bin), GET state (send /state.tsv,
// read-only, for the sync page's stats panel), and PUT (overwrite cards) ----
// Sends one payload via indicate() rather than notify(). notify() is
// fire-and-forget at the ATT level -- the peer never confirms receipt, so a
// silently dropped packet (which does happen; BLE has no guaranteed
// delivery) permanently truncates the transfer with neither side the
// wiser. indicate() waits for the peer's ATT-level confirmation (up to
// BLECharacteristic::indicationTimeout, 1000ms) before returning, so a drop
// surfaces here as a normal, retryable failure instead of silent data loss.
// Also retries with a short backoff if the local send queue is momentarily
// full (see TxCallbacks::onStatus() / notifyCongested below), and always
// yields a little even on the fast path -- this loop can run on the NimBLE
// host task itself (called synchronously from RxCallbacks::onWrite()), so a
// long run with no yield at all risks starving the watchdog. Returns false
// if the client disconnected mid-retry so the caller can give up instead of
// retrying forever.
static bool sendChunkReliably(const uint8_t *data, size_t n) {
  for (;;) {
    if (!bleConnected) return false;
    notifyCongested = false;
    pTxCharacteristic->setValue(data, n);
    pTxCharacteristic->indicate();
    if (notifyCongested) {
      delay(15);   // stack briefly out of send buffers, or peer didn't
      continue;    // confirm in time -- back off and retry this same chunk
    }
    delay(2);
    return true;
  }
}

static void sendFileOverBle(const char *path, uint8_t cmd) {
  File f;
  size_t total = 0;
  if (sdOk) {
    f = SD_MMC.open(path, FILE_READ);
    if (f) total = f.size();
  }

  uint8_t header[5];
  header[0] = cmd;
  header[1] = (uint8_t)(total & 0xFF);
  header[2] = (uint8_t)((total >> 8) & 0xFF);
  header[3] = (uint8_t)((total >> 16) & 0xFF);
  header[4] = (uint8_t)((total >> 24) & 0xFF);
  // This header notify used to be a single unretried notify() call -- if it
  // happened to queue right when the stack was momentarily busy (plausible
  // right after a fresh connection), it was silently dropped, and the app
  // would misparse the first body chunk as the header and reject the whole
  // transfer with "unexpected reply from device". Route it through the same
  // retry path as the body chunks below.
  if (!sendChunkReliably(header, sizeof(header))) {
    Serial.printf("BLE: aborted send (%s), client disconnected\n", path);
    if (f) f.close();
    return;
  }

  static uint8_t chunkBuf[BLE_CHUNK];   // reused across calls; never on the stack
  size_t sent = 0;
  while (f && sent < total) {
    size_t n = minSize(BLE_CHUNK, total - sent);
    if (f.read(chunkBuf, n) != n) {
      Serial.printf("BLE: SD read short (%s) at %u/%u\n", path, (unsigned)sent, (unsigned)total);
      break;
    }
    if (!sendChunkReliably(chunkBuf, n)) {
      Serial.printf("BLE: aborted send (%s), client disconnected\n", path);
      f.close();
      return;
    }
    sent += n;
  }
  if (f) f.close();
  Serial.printf("BLE: sent %u bytes (%s)\n", (unsigned)sent, path);
}

static void sendCardsOverBle() { sendFileOverBle("/cards.bin", 0x01); }
static void sendStateOverBle() { sendFileOverBle("/state.tsv", 0x03); }

static void finishPut() {
  if (putFile) putFile.close();
  Serial.printf("BLE: wrote /cards.bin (%u bytes) -> %s\n",
                (unsigned)rxReceived, putOk ? "ok" : "FAILED");

  uint8_t resp[2] = { 0x02, (uint8_t)(putOk ? 0x00 : 0x01) };
  sendChunkReliably(resp, sizeof(resp));

  rxState = RX_IDLE;
}

// indicate() (unlike notify()) blocks until the peer's ATT-level
// confirmation arrives or indicationTimeout (1000ms) elapses, and reports
// which via onStatus(): SUCCESS_INDICATE means confirmed delivery,
// ERROR_GATT means the local send queue was full, ERROR_INDICATE_TIMEOUT
// means the peer never confirmed (dropped packet, or it's just gone) --
// sendChunkReliably() treats anything but success as "back off and retry".
class TxCallbacks : public BLECharacteristicCallbacks {
  void onStatus(BLECharacteristic *pCharacteristic, Status s, uint32_t code) override {
    if (s != SUCCESS_NOTIFY && s != SUCCESS_INDICATE) notifyCongested = true;
  }
};

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    uint8_t *data = pCharacteristic->getData();
    size_t len = pCharacteristic->getLength();
    if (len == 0) return;

    if (rxState == RX_IDLE) {
      if (data[0] == 0x01) {                       // GET
        pendingAction = PENDING_SEND_CARDS;         // actual send: syncLoop()
      } else if (data[0] == 0x03) {                 // GET state (stats)
        pendingAction = PENDING_SEND_STATE;         // actual send: syncLoop()
      } else if (data[0] == 0x02 && len >= 5) {     // PUT header
        uint32_t total = (uint32_t)data[1] | ((uint32_t)data[2] << 8) |
                          ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
        rxExpected = total;
        rxReceived = 0;
        putOk = sdOk;
        if (putOk) {
          putFile = SD_MMC.open("/cards.bin", FILE_WRITE);   // "w" -> truncates
          if (!putFile) putOk = false;
        }
        if (total == 0) pendingAction = PENDING_FINISH_PUT;   // actual finish: syncLoop()
        else rxState = RX_BODY;
      }
    } else {   // RX_BODY: raw body bytes, no framing -- streamed straight to
               // SD as they arrive rather than buffered in RAM, see putFile.
      size_t n = minSize(len, rxExpected - rxReceived);
      if (putOk && n > 0 && putFile.write(data, n) != n) putOk = false;
      rxReceived += n;
      if (rxReceived >= rxExpected) pendingAction = PENDING_FINISH_PUT;   // actual finish: syncLoop()
    }
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) override {
    bleConnected = true;
    rxState = RX_IDLE;
    if (putFile) putFile.close();   // in case a prior connection dropped mid-PUT
    Serial.println("BLE: central connected");
    // NOTE: used to also call pServer->requestConnParams() here to ask for a
    // shorter connection interval. Removed after it looked like the crash
    // trigger; turned out the real cause was elsewhere (see PendingBleAction
    // above -- notify() called synchronously from a BLE host callback), but
    // calling back into the GAP API from within onConnect() is the same
    // class of risk, so this stays out unless it's revisited deliberately,
    // deferred to syncLoop() the same way sends now are.
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

    // Do the actual BLE send/finish work here, on the main task, instead of
    // inside RxCallbacks::onWrite() -- see PendingBleAction's declaration
    // for why. Snapshot-and-clear before dispatching in case the action
    // itself takes a while and bleConnected/rxState change during it.
    PendingBleAction action = pendingAction;
    pendingAction = PENDING_NONE;
    switch (action) {
      case PENDING_SEND_CARDS: sendCardsOverBle(); break;
      case PENDING_SEND_STATE: sendStateOverBle(); break;
      case PENDING_FINISH_PUT: finishPut(); break;
      case PENDING_NONE: break;
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

  // INDICATE alongside NOTIFY: sendChunkReliably() uses indicate() so every
  // chunk gets a real delivery confirmation instead of the silent-drop risk
  // notify() carries (see sendChunkReliably()'s comment).
  pTxCharacteristic = service->createCharacteristic(
    NUS_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_INDICATE);
  pTxCharacteristic->addDescriptor(new BLE2902());
  pTxCharacteristic->setCallbacks(new TxCallbacks());

  // Plain WRITE (acknowledged): PUT body chunks used to go over WRITE_NR
  // (write-without-response) for speed, but that's just as unacknowledged
  // as notify() was, and it silently dropped data mid-upload in practice
  // (a save would "complete" client-side with no error, but the device
  // never received the full body and so never sent back a save-confirmed
  // ack). Reliability matters more than the extra speed here.
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

// The ROM-level boot banner ("rst:0xc (RTC_SW_CPU_RST)") is genuinely
// ambiguous about *why* the chip reset -- it can mean either a software
// crash/abort or, on some ESP32-S3 revisions, a brownout misreported
// through that same path. esp_reset_reason() is the authoritative,
// unambiguous API for this, so print it plainly at every boot.
static const char *resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON (normal power-up)";
    case ESP_RST_EXT:       return "EXT (external reset pin)";
    case ESP_RST_SW:        return "SW (esp_restart() / ESP.restart())";
    case ESP_RST_PANIC:     return "PANIC (crash: exception/abort)";
    case ESP_RST_INT_WDT:   return "INT_WDT (interrupt watchdog)";
    case ESP_RST_TASK_WDT:  return "TASK_WDT (task watchdog)";
    case ESP_RST_WDT:       return "WDT (other watchdog)";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP (woke from deep sleep)";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (power supply sagged)";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

// ---- Setup / loop ----
void setup() {
  pinMode(PIN_VBAT_PWR, OUTPUT);
  digitalWrite(PIN_VBAT_PWR, HIGH);  // latch battery power first
  pinMode(PIN_PWR_BTN, INPUT_PULLUP);

  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.printf("reset reason: %s\n", resetReasonStr(esp_reset_reason()));
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
