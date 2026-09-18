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
 * Storage is one file per card, /cards/<id>.bin (id is the 8-hex-digit
 * filename) -- so "what cards exist" is just "what files are in /cards/",
 * no separate index/manifest file to fall out of sync with reality. Each
 * file carries its own front/back text, bitmaps, AND box/practiced learning
 * state together, so editing a card can never orphan its progress the way
 * the old cards.bin+state.tsv split (matched by front+back text) could.
 *
 *   /cards/<id>.bin: [magic "FC01"][u32 id LE][u16 box LE][u8 practiced],
 *     then [u16 frontLen LE][frontText UTF-8][u16 backLen LE][backText
 *     UTF-8][frontBitmap][backBitmap], each bitmap BMP_W x BMP_H, 1bpp,
 *     MSB-first, rows padded to a byte -- i.e. exactly what
 *     Adafruit_GFX::drawBitmap() wants. The front/back text is carried
 *     along purely as a courtesy to sync.html's edit UI; the device never
 *     renders it -- sync.html renders both fields to canvas (so the
 *     browser's font/shaping stack, not u8g2, handles Arabic joining,
 *     Vietnamese diacritics, CJK, etc.) and ships pre-dithered pixels. The
 *     in-file id is redundant with the filename and is only used to detect
 *     (and log) a rename/write mismatch -- the filename is what
 *     remove()/rename() actually target, so it's always treated as
 *     authoritative, never the in-file copy.
 *
 * The RX/TX pair carries a tiny framed protocol instead of raw HTTP, since a
 * single BLE attribute value is capped at 512 bytes and the negotiated MTU
 * can be much smaller than a whole card (or the whole deck):
 *
 *   LIST (app -> device): single byte 0x10. Device replies with one notify
 *     [0x10, len:u32 LE] then chunks (BLE_CHUNK bytes each) of the
 *     concatenated per-card records [id:u32 LE][box:u16 LE][practiced:u8]
 *     [frontLen:u16 LE][front][backLen:u16 LE][back] for every card --
 *     everything sync.html's edit UI needs, no bitmaps (sync.html always
 *     re-renders bitmaps from text before any upload, so it never needs the
 *     device's copy).
 *
 *   PUT_CARD (app -> device): one write with [0x11, id:u32 LE, box:u16 LE,
 *     practiced:u8, bodyLen:u32 LE] (id 0 = "assign me a new id"), then as
 *     many writes as needed carrying raw body bytes (chunked at BLE_CHUNK)
 *     -- [frontLen:u16 LE][front][backLen:u16 LE][back][frontBitmap]
 *     [backBitmap] -- until bodyLen bytes have been sent. Device streams
 *     the body straight to /cards/<id>.bin.tmp (never buffers a whole card
 *     in RAM) and atomically renames it into place on success.
 *   PUT_CARD response: one notify [0x11, status, assignedId:u32 LE].
 *     status 0 = saved OK, 1 = error (no SD / write failed).
 *
 *   DELETE_CARD (app -> device): one write [0x12, id:u32 LE].
 *   DELETE_CARD response: one notify [0x12, status].
 *
 *   DELETE_ALL (app -> device): single byte 0x13.
 *   DELETE_ALL response: one notify [0x13, status, deletedCount:u32 LE].
 *
 * setup() calls BLEDevice::setMTU(247) so a fresh connection negotiates
 * enough headroom, though BLE_CHUNK itself is kept at 180 (see its own
 * comment -- 244, the full MTU(247)-3 headroom, was tried and crashed the
 * device for unrelated reasons). It also requests a short (7.5-15ms)
 * connection interval right after connect, and the RX characteristic
 * advertises write-without-response so sync.html can stream PUT bodies
 * without waiting for a per-chunk ATT ack -- see ServerCallbacks::onConnect()
 * and the RX characteristic properties below.
 *
 * Storage split, touch, power-latch and text rendering are unchanged from
 * 006 -- see that file's header for the pin/bus rationale. FONT_BIG/SMALL
 * now only draw fixed ASCII UI chrome (button labels, settings/sync text);
 * card front/back are pre-rendered bitmaps, see the /cards/<id>.bin format
 * above.
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
#include <algorithm>
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
// bitmaps from sync.html, see the /cards/<id>.bin note in the header comment.
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

// Card content: pre-rendered 1bpp bitmaps from sync.html (see /cards/<id>.bin
// in the header comment), blitted at fixed positions instead of drawn as text.
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
// One file per card, /cards/<id>.bin -- see the header comment for the
// on-disk layout. Front/back text isn't kept in RAM at all (nothing
// on-device ever renders it), only the two lengths needed to seek past it
// straight to the bitmaps, cached here so re-reading a card's header isn't
// needed just to draw it or to build a LIST reply.
struct Flashcard {
  uint32_t id;
  int box; bool practiced;
  uint16_t frontLen, backLen;
};
static std::vector<Flashcard> cards;
static int  currentIndex = 0;
static bool sdOk = false;
static uint32_t nextId = 1;   // 0 is the wire-protocol "assign me an id" sentinel

// ---- Per-card file format ----
static const char *CARDS_DIR = "/cards";
static const char *CARD_MAGIC = "FC01";
static const size_t CARD_MAGIC_LEN = 4;
static const size_t CARD_HEADER_BYTES = 4 + 4 + 2 + 1;   // magic + id + box + practiced

// Formats "/cards/XXXXXXXX.bin" -- 8 hex digits, zero-padded, so the
// basename is exactly 8.3 (8 name chars + ".bin") regardless of whether the
// FAT driver here has long-filename support.
static void cardFilePath(uint32_t id, char *out, size_t outLen) {
  snprintf(out, outLen, "%s/%08X.bin", CARDS_DIR, (unsigned)id);
}

// Parses the 8-hex-digit id back out of a name as returned by File::name()
// (which may or may not include the "/cards/" prefix depending on core
// version -- handle both by scanning from the last '/'). Returns false for
// anything that doesn't look like one of ours, so callers can skip stray
// junk files instead of misparsing them.
static bool parseIdFromFilename(const char *name, uint32_t &outId) {
  const char *slash = strrchr(name, '/');
  const char *base = slash ? slash + 1 : name;
  if (strlen(base) != 12 || strcmp(base + 8, ".bin") != 0) return false;
  char *end = nullptr;
  unsigned long v = strtoul(base, &end, 16);
  if (end != base + 8) return false;
  outId = (uint32_t)v;
  return true;
}

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

// PUT_CARD bodies (and LIST responses, see sendListOverBle() below) are
// streamed straight to/from the SD card in BLE_CHUNK-sized pieces rather
// than buffered whole in RAM -- buffering a whole deck through a
// std::vector on top of everything else this device already holds is
// exactly the kind of heap pressure that caused a crash right after a large
// save/load completed under the old whole-blob protocol (confirmed: SD
// write and BLE ack both logged success, then a reboot). A single card is
// small (a couple KB) so the risk is much lower now, but streaming costs
// nothing extra and keeps one consistent pattern.
enum RxState { RX_IDLE, RX_BODY };
static RxState rxState = RX_IDLE;
static size_t rxExpected = 0;
static size_t rxReceived = 0;
static File putFile;         // open across a PUT_CARD's onWrite() calls
static bool putOk = false;   // false if sdOk was false, open failed, or a write came up short
static uint32_t putCardId = 0;
static int      putCardBox = 0;
static bool     putCardPracticed = false;
static uint32_t pendingDeleteId = 0;

// RxCallbacks::onWrite() runs on the NimBLE host task, inside that task's
// own (comparatively small) stack frame for the callback. Calling back into
// the BLE stack from there -- e.g. a loop of retried notify() calls to send
// a whole file -- is a documented cause of stack overflow crashes on this
// stack (confirmed here: reliably crashed right after connect, regardless
// of chunk size or connection params, with no visible backtrace because a
// stack overflow corrupts the very stack the panic handler would unwind).
// So onWrite() only ever sets one of these flags (or streams raw PUT_CARD
// bytes to an already-open File, which doesn't touch the BLE stack); the
// actual send/finish/delete work happens in syncLoop() on the main Arduino
// task instead, which has a normal-sized stack and isn't nested inside a
// BLE callback at all.
enum PendingBleAction {
  PENDING_NONE, PENDING_SEND_LIST, PENDING_FINISH_PUT_CARD,
  PENDING_DELETE_CARD, PENDING_DELETE_ALL
};
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

// Reads one card's bitmap from /cards/<id>.bin into `bmpBuf` and blits it.
// No-op (leaves the area blank) on any SD hiccup rather than drawing
// garbage. Defined here, ahead of loadCards()/the SD-backed Flashcard struct
// below, only to sit next to the screen-drawing functions that call it.
// `frontLen`/`backLen` (cached on the Flashcard at load time) let this seek
// straight to the wanted bitmap without re-reading the header.
static uint8_t bmpBuf[BMP_BYTES];
static void drawCardBitmap(const Flashcard &c, bool front, int x, int y) {
  char path[24];
  cardFilePath(c.id, path, sizeof(path));
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return;
  uint32_t offset = CARD_HEADER_BYTES + 2 + c.frontLen + 2 + c.backLen;
  if (!front) offset += BMP_BYTES;
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
      drawCardBitmap(cards[currentIndex], true, FRONT_BMP_X, FRONT_BMP_Y);

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

    drawCardBitmap(cards[currentIndex], true, FRONT_BMP_X, FRONT_BMP_Y);
    drawDashedLine(DASH_Y);
    drawCardBitmap(cards[currentIndex], false, BACK_BMP_X, BACK_BMP_Y);

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
// Reads a u16-LE length prefix, then skips that many bytes without keeping
// them -- used to walk past front/back text we don't need in RAM, while
// still learning how long it was (cached on the Flashcard for later seeks).
static bool readLenPrefixedSkip(File &f, uint16_t &outLen) {
  uint8_t lenBuf[2];
  if (f.read(lenBuf, 2) != 2) return false;
  outLen = lenBuf[0] | ((uint16_t)lenBuf[1] << 8);
  return f.seek(f.position() + outLen);
}

// Parses one already-open card file's header + record shape into `outCard`.
// Validates the record walks exactly to EOF (catches a truncated/corrupt
// file, same defensive spirit the old cards.bin loader had) before
// accepting it.
static bool loadOneCard(File &f, uint32_t idFromName, Flashcard &outCard) {
  uint8_t hdr[CARD_HEADER_BYTES];
  if (f.read(hdr, CARD_HEADER_BYTES) != CARD_HEADER_BYTES ||
      memcmp(hdr, CARD_MAGIC, CARD_MAGIC_LEN) != 0) {
    return false;
  }
  uint32_t idInFile = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
                       ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
  if (idInFile != idFromName) {
    Serial.printf("cards/%08X.bin: id mismatch (file says %08X), trusting filename\n",
                  (unsigned)idFromName, (unsigned)idInFile);
  }
  outCard.id = idFromName;
  outCard.box = hdr[8] | (hdr[9] << 8);
  outCard.practiced = hdr[10] != 0;

  if (!readLenPrefixedSkip(f, outCard.frontLen)) return false;
  if (!readLenPrefixedSkip(f, outCard.backLen)) return false;
  if (!f.seek(f.position() + 2 * BMP_BYTES)) return false;
  return f.position() == f.size();
}

// Loads every card by scanning /cards/ -- "what cards exist" is just
// "what files are in the directory," so there's no separate index/manifest
// to fall out of sync with reality, and nothing to reconcile against a
// second file the way the old cards.bin+state.tsv split needed.
static bool loadCards() {
  cards.clear();
  uint32_t maxId = 0;

  File dir = SD_MMC.open(CARDS_DIR);
  if (!dir || !dir.isDirectory()) {
    Serial.println("no /cards directory yet");
    if (dir) dir.close();
    nextId = 1;
    return false;
  }

  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory()) { f.close(); continue; }
    uint32_t idFromName;
    if (!parseIdFromFilename(f.name(), idFromName)) {
      Serial.printf("cards: skipping unrecognized file %s\n", f.name());
      f.close();
      continue;
    }
    Flashcard c;
    bool ok = loadOneCard(f, idFromName, c);
    f.close();
    if (!ok) {
      Serial.printf("cards/%08X.bin: bad/truncated record, skipping\n", (unsigned)idFromName);
      continue;
    }
    cards.push_back(c);
    if (idFromName > maxId) maxId = idFromName;
  }
  dir.close();

  // openNextFile() order isn't id order; sort so "deck order" (used by
  // buildSessionQueue()'s new-card selection below) means creation order,
  // matching the old format's append-order behavior.
  std::sort(cards.begin(), cards.end(),
            [](const Flashcard &a, const Flashcard &b) { return a.id < b.id; });

  nextId = maxId + 1;
  Serial.printf("loaded %u card(s), nextId=%u\n", (unsigned)cards.size(), (unsigned)nextId);
  return !cards.empty();
}

// Atomically rewrites one card's box/practiced; front/back text and bitmaps
// are copied through byte-for-byte unchanged (a grade never touches them).
// Same temp-file+rename pattern the old whole-deck saveState() used, just
// scoped to a single small file now, so a crash mid-write can never lose
// more than the one card being graded.
static bool saveCard(const Flashcard &c) {
  if (!sdOk) return false;
  char path[24], tmpPath[28];
  cardFilePath(c.id, path, sizeof(path));
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);

  File src = SD_MMC.open(path, FILE_READ);
  if (!src) { Serial.printf("saveCard %08X: source missing\n", (unsigned)c.id); return false; }

  SD_MMC.remove(tmpPath);   // stale leftover from an interrupted save, if any
  File dst = SD_MMC.open(tmpPath, FILE_WRITE);
  if (!dst) { src.close(); Serial.println("saveCard: open tmp FAILED"); return false; }

  uint8_t hdr[CARD_HEADER_BYTES];
  memcpy(hdr, CARD_MAGIC, CARD_MAGIC_LEN);
  hdr[4] = (uint8_t)(c.id);         hdr[5] = (uint8_t)(c.id >> 8);
  hdr[6] = (uint8_t)(c.id >> 16);   hdr[7] = (uint8_t)(c.id >> 24);
  hdr[8] = (uint8_t)(c.box);        hdr[9] = (uint8_t)(c.box >> 8);
  hdr[10] = c.practiced ? 1 : 0;
  bool ok = (dst.write(hdr, CARD_HEADER_BYTES) == CARD_HEADER_BYTES);

  src.seek(CARD_HEADER_BYTES);   // skip the old header, copy everything after unchanged
  static uint8_t copyBuf[128];
  while (ok && src.available()) {
    size_t n = src.read(copyBuf, sizeof(copyBuf));
    if (n == 0) break;
    ok = (dst.write(copyBuf, n) == n);
  }

  dst.flush();
  dst.close();
  src.close();

  if (!ok) { SD_MMC.remove(tmpPath); return false; }
  SD_MMC.remove(path);
  if (!SD_MMC.rename(tmpPath, path)) {
    Serial.printf("saveCard %08X: rename FAILED\n", (unsigned)c.id);
    return false;
  }
  return true;
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
  saveCard(cards[idx]);

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
  saveCard(cards[idx]);

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

// ---- BLE protocol: LIST (send every card's id/box/practiced/text),
// PUT_CARD (create/overwrite one card), DELETE_CARD, DELETE_ALL ----
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

// LIST: streams id+box+practiced+front+back text for every loaded card,
// same header-then-chunks shape the old whole-file sender used, but
// assembled from many small per-card file reads instead of one path (no
// bitmaps -- sync.html always re-renders those from text before any
// upload, so it never needs the device's copy).
static void sendListOverBle() {
  size_t total = 0;
  for (auto &c : cards) total += 4 + 2 + 1 + 2 + c.frontLen + 2 + c.backLen;

  uint8_t header[5];
  header[0] = 0x10;
  header[1] = (uint8_t)(total & 0xFF);
  header[2] = (uint8_t)((total >> 8) & 0xFF);
  header[3] = (uint8_t)((total >> 16) & 0xFF);
  header[4] = (uint8_t)((total >> 24) & 0xFF);
  if (!sendChunkReliably(header, sizeof(header))) {
    Serial.println("BLE: LIST aborted, client disconnected");
    return;
  }

  static uint8_t chunkBuf[BLE_CHUNK];   // reused across calls; never on the stack
  size_t chunkLen = 0;
  // Buffers small per-card pieces into BLE_CHUNK-sized sends instead of one
  // send per field, same batching the old single-file sender got for free
  // by reading BLE_CHUNK bytes at a time.
  auto flushChunk = [&]() -> bool {
    if (chunkLen == 0) return true;
    bool ok = sendChunkReliably(chunkBuf, chunkLen);
    chunkLen = 0;
    return ok;
  };
  auto feed = [&](const uint8_t *data, size_t n) -> bool {
    while (n > 0) {
      size_t take = minSize(BLE_CHUNK - chunkLen, n);
      memcpy(chunkBuf + chunkLen, data, take);
      chunkLen += take; data += take; n -= take;
      if (chunkLen == BLE_CHUNK && !flushChunk()) return false;
    }
    return true;
  };

  size_t sent = 0;
  static uint8_t textBuf[128];
  for (auto &c : cards) {
    char path[24];
    cardFilePath(c.id, path, sizeof(path));
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) { Serial.printf("BLE: LIST skip %08X, open failed\n", (unsigned)c.id); continue; }

    uint8_t rec[9];
    rec[0] = (uint8_t)(c.id);       rec[1] = (uint8_t)(c.id >> 8);
    rec[2] = (uint8_t)(c.id >> 16); rec[3] = (uint8_t)(c.id >> 24);
    rec[4] = (uint8_t)(c.box);      rec[5] = (uint8_t)(c.box >> 8);
    rec[6] = c.practiced ? 1 : 0;
    rec[7] = (uint8_t)(c.frontLen); rec[8] = (uint8_t)(c.frontLen >> 8);
    if (!feed(rec, sizeof(rec))) { f.close(); flushChunk(); return; }

    f.seek(CARD_HEADER_BYTES + 2);   // front text starts right after its length prefix
    uint16_t remaining = c.frontLen;
    bool ok = true;
    while (ok && remaining > 0) {
      size_t n = minSize(sizeof(textBuf), remaining);
      ok = (f.read(textBuf, n) == n) && feed(textBuf, n);
      remaining -= n;
    }
    if (!ok) { f.close(); flushChunk(); return; }

    uint8_t backLenBytes[2] = { (uint8_t)(c.backLen), (uint8_t)(c.backLen >> 8) };
    if (!feed(backLenBytes, sizeof(backLenBytes))) { f.close(); flushChunk(); return; }
    f.seek(CARD_HEADER_BYTES + 2 + c.frontLen + 2);   // back text
    remaining = c.backLen;
    while (ok && remaining > 0) {
      size_t n = minSize(sizeof(textBuf), remaining);
      ok = (f.read(textBuf, n) == n) && feed(textBuf, n);
      remaining -= n;
    }
    f.close();
    if (!ok) { flushChunk(); return; }

    sent += 4 + 2 + 1 + 2 + c.frontLen + 2 + c.backLen;
  }
  flushChunk();
  Serial.printf("BLE: sent LIST, %u bytes / %u card(s)\n", (unsigned)sent, (unsigned)cards.size());
}

static void upsertCardInMemory(uint32_t id, int box, bool practiced,
                                uint16_t frontLen, uint16_t backLen) {
  for (auto &c : cards) {
    if (c.id == id) {
      c.box = box; c.practiced = practiced; c.frontLen = frontLen; c.backLen = backLen;
      return;
    }
  }
  Flashcard c;
  c.id = id; c.box = box; c.practiced = practiced;
  c.frontLen = frontLen; c.backLen = backLen;
  cards.push_back(c);
}

static void finishPutCard() {
  if (putFile) putFile.close();

  char path[24], tmpPath[28];
  cardFilePath(putCardId, path, sizeof(path));
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
  if (putOk) {
    SD_MMC.remove(path);
    putOk = SD_MMC.rename(tmpPath, path);
  }
  if (!putOk) SD_MMC.remove(tmpPath);

  Serial.printf("BLE: wrote card %08X (%u bytes) -> %s\n",
                (unsigned)putCardId, (unsigned)rxReceived, putOk ? "ok" : "FAILED");

  uint16_t frontLen = 0, backLen = 0;
  if (putOk) {
    // Peek the two lengths back out of the file we just committed, rather
    // than threading them through from the raw PUT_CARD body bytes, so the
    // in-RAM cache always matches what's actually on disk.
    File f = SD_MMC.open(path, FILE_READ);
    if (f) {
      f.seek(CARD_HEADER_BYTES);
      readLenPrefixedSkip(f, frontLen);
      readLenPrefixedSkip(f, backLen);
      f.close();
    }
    upsertCardInMemory(putCardId, putCardBox, putCardPracticed, frontLen, backLen);
  }

  uint8_t resp[6];
  resp[0] = 0x11;
  resp[1] = putOk ? 0 : 1;
  resp[2] = (uint8_t)(putCardId);       resp[3] = (uint8_t)(putCardId >> 8);
  resp[4] = (uint8_t)(putCardId >> 16); resp[5] = (uint8_t)(putCardId >> 24);
  sendChunkReliably(resp, sizeof(resp));

  rxState = RX_IDLE;
}

static void deleteCardOnDevice(uint32_t id) {
  char path[24];
  cardFilePath(id, path, sizeof(path));
  bool ok = SD_MMC.remove(path);
  if (ok) {
    for (size_t i = 0; i < cards.size(); i++) {
      if (cards[i].id == id) { cards.erase(cards.begin() + i); break; }
    }
  }
  Serial.printf("BLE: delete card %08X -> %s\n", (unsigned)id, ok ? "ok" : "FAILED");
  uint8_t resp[2] = { 0x12, (uint8_t)(ok ? 0 : 1) };
  sendChunkReliably(resp, sizeof(resp));
}

static void deleteAllCardsOnDevice() {
  uint32_t deleted = 0;
  File dir = SD_MMC.open(CARDS_DIR);
  if (dir && dir.isDirectory()) {
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      bool isDirEntry = f.isDirectory();
      uint32_t id;
      bool named = !isDirEntry && parseIdFromFilename(f.name(), id);
      f.close();
      if (!named) continue;
      char path[24];
      cardFilePath(id, path, sizeof(path));
      if (SD_MMC.remove(path)) deleted++;
    }
    dir.close();
  }
  cards.clear();
  nextId = 1;
  Serial.printf("BLE: delete all -> %u file(s) removed\n", (unsigned)deleted);

  uint8_t resp[6];
  resp[0] = 0x13; resp[1] = 0;
  resp[2] = (uint8_t)(deleted);       resp[3] = (uint8_t)(deleted >> 8);
  resp[4] = (uint8_t)(deleted >> 16); resp[5] = (uint8_t)(deleted >> 24);
  sendChunkReliably(resp, sizeof(resp));
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
      if (data[0] == 0x10) {                        // LIST
        pendingAction = PENDING_SEND_LIST;          // actual send: syncLoop()
      } else if (data[0] == 0x11 && len >= 12) {    // PUT_CARD header
        uint32_t reqId = (uint32_t)data[1] | ((uint32_t)data[2] << 8) |
                          ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
        putCardBox = data[5] | (data[6] << 8);
        putCardPracticed = data[7] != 0;
        uint32_t bodyLen = (uint32_t)data[8] | ((uint32_t)data[9] << 8) |
                            ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);
        putCardId = (reqId == 0) ? nextId++ : reqId;
        rxExpected = bodyLen;
        rxReceived = 0;
        putOk = sdOk;
        if (putOk) {
          char path[24], tmpPath[28];
          cardFilePath(putCardId, path, sizeof(path));
          snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
          SD_MMC.remove(tmpPath);
          putFile = SD_MMC.open(tmpPath, FILE_WRITE);
          if (!putFile) {
            putOk = false;
          } else {
            uint8_t hdr[CARD_HEADER_BYTES];
            memcpy(hdr, CARD_MAGIC, CARD_MAGIC_LEN);
            hdr[4] = (uint8_t)(putCardId);       hdr[5] = (uint8_t)(putCardId >> 8);
            hdr[6] = (uint8_t)(putCardId >> 16); hdr[7] = (uint8_t)(putCardId >> 24);
            hdr[8] = (uint8_t)(putCardBox);      hdr[9] = (uint8_t)(putCardBox >> 8);
            hdr[10] = putCardPracticed ? 1 : 0;
            putOk = (putFile.write(hdr, CARD_HEADER_BYTES) == CARD_HEADER_BYTES);
          }
        }
        if (bodyLen == 0) pendingAction = PENDING_FINISH_PUT_CARD;   // actual finish: syncLoop()
        else rxState = RX_BODY;
      } else if (data[0] == 0x12 && len >= 5) {     // DELETE_CARD
        pendingDeleteId = (uint32_t)data[1] | ((uint32_t)data[2] << 8) |
                           ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
        pendingAction = PENDING_DELETE_CARD;        // actual delete: syncLoop()
      } else if (data[0] == 0x13) {                 // DELETE_ALL
        pendingAction = PENDING_DELETE_ALL;         // actual delete: syncLoop()
      }
    } else {   // RX_BODY: raw body bytes, no framing -- streamed straight to
               // the temp file as they arrive rather than buffered in RAM,
               // see putFile.
      size_t n = minSize(len, rxExpected - rxReceived);
      if (putOk && n > 0 && putFile.write(data, n) != n) putOk = false;
      rxReceived += n;
      if (rxReceived >= rxExpected) pendingAction = PENDING_FINISH_PUT_CARD;   // actual finish: syncLoop()
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
      case PENDING_SEND_LIST:       sendListOverBle(); break;
      case PENDING_FINISH_PUT_CARD: finishPutCard(); break;
      case PENDING_DELETE_CARD:     deleteCardOnDevice(pendingDeleteId); break;
      case PENDING_DELETE_ALL:      deleteAllCardsOnDevice(); break;
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

  // FILE_WRITE never creates missing parent directories, so a fresh/wiped
  // SD card with no /cards/ yet would make the very first PUT_CARD fail to
  // open its temp file and report a save error -- make sure it exists
  // before anything tries to write into it.
  if (!SD_MMC.exists(CARDS_DIR)) SD_MMC.mkdir(CARDS_DIR);

  loadCards();

  randomSeed(esp_random());
  buildSessionQueue();
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
