// Pin map, panel geometry, screen layout, touch zones and tuning constants
// shared by every module in this sketch. Header-only, all constexpr.
#pragma once
#include <Arduino.h>

// ---- Pin map (Waveshare user_config.h + 04_SD_Card/sdcard_bsp.cpp) ----
constexpr int PIN_TP_SDA  = 47;
constexpr int PIN_TP_SCL  = 48;
constexpr int PIN_TP_INT  = 21;   // active low; unused (we poll)
constexpr int PIN_TP_RST  = 7;
constexpr int PIN_EPD_PWR = 6;    // e-paper power rail, active LOW

constexpr int PIN_VBAT_PWR = 17;  // battery self-latch (see 005 notes)
constexpr int PIN_PWR_BTN  = 18;  // active LOW
constexpr uint32_t PWR_LONGPRESS_MS = 1000;

constexpr int PIN_EPD_CS   = 11;
constexpr int PIN_EPD_DC   = 10;
constexpr int PIN_EPD_RST  = 9;
constexpr int PIN_EPD_BUSY = 8;
constexpr int PIN_EPD_SCK  = 12;
constexpr int PIN_EPD_MOSI = 13;

// SD card on the SDMMC peripheral, 1-bit mode.
constexpr int PIN_SD_CLK = 39;
constexpr int PIN_SD_CMD = 41;
constexpr int PIN_SD_D0  = 40;

constexpr uint8_t FT6336_ADDR = 0x38;

// ---- Orientation calibration (validated in 003) ----
constexpr bool SWAP_XY = false;
constexpr bool FLIP_X  = false;
constexpr bool FLIP_Y  = false;

// ---- Panel geometry ----
constexpr int W = 200, H = 200;
constexpr int MID_X = W / 2;

// ---- Screen layout ----
//   y   0.. 7   top icon strip      reload (left)              connect (right)
//   y   8..99   FRONT               front bitmap, full width
//   y 100..191  BACK                back bitmap (after reveal); y=100 doubles
//                                   as the dashed separator (bitmaps keep
//                                   their first row blank)
//   y 192..199  bottom icon strip   prompt: reveal (centre)
//                                   answer: wrong (left)  correct (right)
constexpr int ICON_PX       = 8;
constexpr int ICON_MARGIN_X = 2;
constexpr int STRIP_H       = ICON_PX;

constexpr int TOP_STRIP_Y    = 0;
constexpr int BOTTOM_STRIP_Y = H - STRIP_H;                       // 192

constexpr int CONTENT_Y      = STRIP_H;                           // 8
constexpr int CONTENT_H      = H - 2 * STRIP_H;                   // 184
constexpr int FRONT_Y        = CONTENT_Y;                         // 8
constexpr int BACK_Y         = CONTENT_Y + CONTENT_H / 2;         // 100
constexpr int HALF_H         = CONTENT_H / 2;                     // 92

constexpr int ICON_LEFT_X    = ICON_MARGIN_X;
constexpr int ICON_RIGHT_X   = W - ICON_MARGIN_X - ICON_PX;
constexpr int ICON_CENTER_X  = MID_X - ICON_PX / 2;

// Card bitmaps: one half of the content area each, pre-rendered by sync.html
// (see /cards/<id>.bin in card_store.h). Must match BMP_W/BMP_H in sync.html.
constexpr int BMP_W = W, BMP_H = HALF_H;
constexpr int BMP_ROW_BYTES = (BMP_W + 7) / 8;
constexpr size_t BMP_BYTES = (size_t)BMP_ROW_BYTES * BMP_H;

// ---- Touch zones ----
// Icons are ~1.5 mm, so the top-corner zones are much bigger than the glyphs
// (they overlap the front area, which is otherwise inert). Kept smaller than
// the old 64 px gear zone: a stray tap on "connect" means a Restart to leave.
constexpr int CORNER_ZONE = 40;
// The whole lower half (empty back area + bottom strip) is the action zone:
// reveal before the flip, wrong (left) / correct (right) after.
constexpr int ACTION_ZONE_Y = BACK_Y;

// ---- Sync screen ----
constexpr int SYNC_TITLE_Y = 106;                 // baseline of "Sync Mode" (text centre ~ y=100)
constexpr int SYNC_RST_W = 120, SYNC_RST_H = 32;
constexpr int SYNC_RST_X = MID_X - SYNC_RST_W / 2;
constexpr int SYNC_RST_Y = 124;

// ---- Scheduler ----
constexpr int NEW_CARDS_PER_SESSION = 12;
constexpr int BUBBLE_PASSES         = 2;
constexpr int INCORRECT_DELAY_MIN   = 4;   // reappear in 4..7 trials
constexpr int INCORRECT_DELAY_MAX   = 7;

// ---- BLE (Nordic UART Service) ----
#define DEVICE_NAME      "Flashcards"
#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// Payload bytes per notify/write. Must match CHUNK in sync.html. 244 (full
// MTU(247)-3 headroom) was tried and crashed the device -- the real cause was
// BLE calls from inside onWrite() (see PendingBleAction in ble_sync.cpp) --
// but 180 is the known-stable value, so it stays.
constexpr size_t BLE_CHUNK = 180;

// Compile-time transfer diagnostics. When 0, the firmware timing counters and
// serial summary are omitted entirely. Keep per-chunk logging out of callbacks:
// it would change the timing we are trying to measure.
#define BLE_TIMING_DEBUG 1

// Versioned fast PUT protocol. This is a production feature, not diagnostic
// code; set to 0 only to build firmware that exposes the legacy protocol.
#define BLE_FAST_SYNC 1
