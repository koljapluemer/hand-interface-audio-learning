// Session queue over `cards` (by index): built once per boot, then advanced
// by grading. Box/practiced persist to SD immediately; queue position is
// session-only.
#pragma once
#include "card_store.h"

void buildSessionQueue();
const Flashcard *currentCard();   // nullptr when the deck is empty
void gradeCorrect();              // advance to the next card
void gradeIncorrect();            // advance; same card reappears in 4..7 trials
