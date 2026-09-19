// Everything that touches the e-paper panel. Owns `display` and the u8g2
// text renderer; nothing else in the sketch includes GxEPD2.
//
// Refresh policy: normal flashcard operation is partial-window updates only.
// A full refresh (flashing, clears ghosting) happens only for the boot draw
// (forced by GxEPD2's initial-refresh flag), entering sync mode, the user's
// hard-refresh button, and the one-off "Bye!" / "No SD card" screens.
#pragma once
#include "card_store.h"

enum Refresh { REFRESH_PARTIAL, REFRESH_FULL };

// Which rows a partial update redraws (always full width). Ignored for
// REFRESH_FULL, which redraws the whole screen.
enum Region {
  REGION_BELOW_TOP,    // front + back + bottom strip: y 8..199 (card change)
  REGION_LOWER_HALF,   // back + bottom strip: y 100..199 (reveal)
};

void uiPowerOn();    // e-paper power rail on; call before touchBegin()
void uiBegin();      // SPI, panel init, text renderer

// Draws the flashcard screen: top icons, front, separator, and -- when
// `revealed` -- the back, then the bottom icons for that phase. A null
// `card` shows the empty-deck hint. Elements outside the dirty window are
// skipped, so no SD bitmap is read for rows that won't be shown.
void uiRenderCard(const Flashcard *card, bool revealed, Region region, Refresh mode);

void uiRenderSync();      // full refresh: "Sync Mode" + Restart button
void uiFlashRestart();    // instant partial-refresh ack for the Restart tap
void uiRenderFatalSd();
void uiRenderBye();
