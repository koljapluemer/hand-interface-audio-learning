/*
 * 007_esp_s3_touch_flashcard_ble
 *
 * Board: Waveshare ESP32-S3-ePaper-1.54
 *        (200x200 black/white e-paper, FT6336 capacitive touch, I2C 0x38,
 *         microSD on the ESP32-S3 SDMMC peripheral)
 *
 * {front, back} flashcards stored on SD, synced from the companion page
 * sync.html (Web Bluetooth) over a Nordic UART Service. Card front/back are
 * 200x92 bitmaps pre-rendered by sync.html; the device never renders card text.
 *
 * Screen (see config.h for the exact geometry):
 *
 *     y   0.. 7  [reload]                              [connect]
 *     y   8..99  FRONT of the card
 *     y 100..191 BACK of the card (blank until revealed)
 *     y 192..199 prompt: [reveal]      answer: [wrong]   [correct]
 *
 *   - top-left  : hard (full) refresh, to clear e-paper ghosting on demand
 *   - top-right : enter sync mode
 *   - lower half: the whole (empty) back area is the reveal button; after the
 *                 flip its left half is "wrong", its right half "correct"
 *   Icons are tiny, so their touch zones are much larger (config.h).
 *
 * State machine (main loop; one tap -> one Action -> at most one transition):
 *
 *   state      action        next state   refresh
 *   ---------  ------------  -----------  ----------------------------------
 *   PROMPT     REVEAL        ANSWER       partial: back + bottom strip
 *   ANSWER     WRONG/CORRECT PROMPT       partial: front + back + bottom strip
 *   PROMPT     HARD_REFRESH  PROMPT       FULL redraw of the current view
 *   ANSWER     HARD_REFRESH  ANSWER       FULL redraw of the current view
 *   PROMPT/    ENTER_SYNC    SYNC         FULL: "Sync Mode" + Restart, then BLE
 *   ANSWER
 *   SYNC       RESTART       (reboot)     brief inverted-button ack, restart
 *
 * An empty deck is a view of PROMPT (REVEAL is ignored), not a separate state.
 * Normal flashcard operation is partial-refresh only; full refreshes happen
 * only at boot (forced by GxEPD2), on HARD_REFRESH, on ENTER_SYNC, and for the
 * "Bye!" / "No SD card" screens. The game is paused while in SYNC. Long-press
 * PWR shuts the device down from any state.
 *
 * Modules: config.h (pins/layout/constants), touch, power, ui (all display
 * code), card_store (per-card SD files), scheduler (session queue), ble_sync
 * (protocol + server; see ble_sync.h), icons.h (generated from icons/*.png by
 * icons/gen_icons.py).
 *
 * Libraries: GxEPD2, U8g2_for_Adafruit_GFX, the ESP32 core's bundled BLE
 * library, SD_MMC (core >= 2.0.7 for SD_MMC.setPins()). Arduino IDE settings:
 * same as 003_esp_s3_touch_quadrants_on_serial; see the README for the
 * arduino-cli FQBN.
 */

#include <Arduino.h>
#include "esp_system.h"   // esp_reset_reason()
#include "config.h"
#include "touch.h"
#include "power.h"
#include "ui.h"
#include "card_store.h"
#include "scheduler.h"
#include "ble_sync.h"

enum AppState { ST_PROMPT, ST_ANSWER, ST_SYNC };
enum Action {
  ACT_NONE, ACT_REVEAL, ACT_WRONG, ACT_CORRECT,
  ACT_HARD_REFRESH, ACT_ENTER_SYNC, ACT_RESTART
};

static AppState state = ST_PROMPT;

// Pure tap -> action mapping. Corner icons win over the lower-half zones.
static Action hitTest(AppState s, int x, int y) {
  if (s == ST_SYNC) {
    return inRect(x, y, SYNC_RST_X, SYNC_RST_Y, SYNC_RST_W, SYNC_RST_H) ? ACT_RESTART : ACT_NONE;
  }
  if (y < CORNER_ZONE && x < CORNER_ZONE)     return ACT_HARD_REFRESH;
  if (y < CORNER_ZONE && x >= W - CORNER_ZONE) return ACT_ENTER_SYNC;
  if (y >= ACTION_ZONE_Y) {
    if (s == ST_PROMPT) return currentCard() ? ACT_REVEAL : ACT_NONE;
    return x < MID_X ? ACT_WRONG : ACT_CORRECT;
  }
  return ACT_NONE;
}

static void apply(Action a) {
  switch (a) {
    case ACT_REVEAL:
      state = ST_ANSWER;
      uiRenderCard(currentCard(), true, REGION_LOWER_HALF, REFRESH_PARTIAL);
      break;
    case ACT_WRONG:
    case ACT_CORRECT:
      Serial.println(a == ACT_WRONG ? "-> Wrong" : "-> Correct");
      if (a == ACT_WRONG) gradeIncorrect(); else gradeCorrect();
      state = ST_PROMPT;
      uiRenderCard(currentCard(), false, REGION_BELOW_TOP, REFRESH_PARTIAL);
      break;
    case ACT_HARD_REFRESH:
      Serial.println("-> hard refresh");
      uiRenderCard(currentCard(), state == ST_ANSWER, REGION_BELOW_TOP, REFRESH_FULL);
      break;
    case ACT_ENTER_SYNC:
      state = ST_SYNC;
      uiRenderSync();      // before BLE init, so the tap gets immediate feedback
      bleSyncStart();
      break;
    case ACT_RESTART:
      uiFlashRestart();
      delay(300);
      ESP.restart();
      break;
    case ACT_NONE:
      break;
  }
}

// esp_reset_reason() is the authoritative answer to "why did the chip reset"
// (the ROM boot banner's RTC_SW_CPU_RST is ambiguous between a crash and a
// misreported brownout), so print it plainly at every boot.
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

void setup() {
  powerBegin();   // latch battery power first

  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.printf("reset reason: %s\n", resetReasonStr(esp_reset_reason()));
  Serial.println("007_esp_s3_touch_flashcard_ble");

  uiPowerOn();
  touchBegin();
  uiBegin();

  if (!sdBegin()) {
    uiRenderFatalSd();
    while (true) { handlePowerButton(); delay(50); }
  }

  loadCards();
  randomSeed(esp_random());
  buildSessionQueue();

  state = ST_PROMPT;
  // Must be an explicit full draw: GxEPD2 forces a full *refresh* on the first
  // update, but only of what was written to RAM, so a partial window here
  // would leave the rows outside it (the top icon strip) blank.
  uiRenderCard(currentCard(), false, REGION_BELOW_TOP, REFRESH_FULL);

  Serial.println("top-left = hard refresh, top-right = sync; lower half = reveal / wrong / correct");
}

void loop() {
  handlePowerButton();
  if (state == ST_SYNC) bleSyncTick();

  int x, y;
  if (pollTapDown(x, y)) {
    Action a = hitTest(state, x, y);
    Serial.printf("tap (%3d,%3d) state=%d action=%d\n", x, y, (int)state, (int)a);
    apply(a);
  }

  delay(state == ST_SYNC ? 10 : 30);   // ~33 Hz poll in play; also debounces
}
