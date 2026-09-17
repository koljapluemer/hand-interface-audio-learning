/*
 * fsrs.h -- FSRS-6 scheduling math, hand-ported from the official py-fsrs
 * reference implementation (open-spaced-repetition/py-fsrs, scheduler.py).
 * Verified against that implementation's own output -- see fsrs_test.cpp.
 *
 * This is only the scheduler: given a card's current memory state and a
 * review rating, compute its next memory state and due interval. It does
 * NOT include the FSRS optimizer (the part that trains w[] from a user's
 * review history via gradient descent) -- that needs a real ML stack and
 * isn't something a personal device should be doing. Instead this uses the
 * published default weights, which are calibrated on a large aggregate
 * dataset and are FSRS's own recommended starting point before you have
 * enough review history to justify optimizing your own.
 *
 * Deliberately NOT ported from py-fsrs:
 *   - Learning/Relearning states with sub-day step queues (e.g. "again ->
 *     show again in 10 minutes"). Every review here goes straight through
 *     the day-granularity Review-state math -- appropriate for a device
 *     with no reliable sub-day clock and a game loop that doesn't want to
 *     re-show a card seconds after answering it.
 *   - Interval fuzzing (randomizing +/- a few percent so cards don't clump).
 *     Can be added later; omitted so this module is deterministic and easy
 *     to test.
 *
 * No Arduino/ESP dependency -- only <math.h> and <stdint.h> -- so this
 * compiles and can be unit-tested on a desktop toolchain too.
 */
#pragma once

#include <stdint.h>

namespace fsrs {

enum Rating : uint8_t { AGAIN = 1, HARD = 2, GOOD = 3, EASY = 4 };

struct MemoryState {
  float stability;   // S, in days
  float difficulty;  // D, in [1, 10]
};

// FSRS-6 default parameters (21 weights), from py-fsrs's DEFAULT_PARAMETERS.
extern const float DEFAULT_WEIGHTS[21];

constexpr float DEFAULT_REQUEST_RETENTION = 0.9f;
constexpr uint32_t DEFAULT_MAXIMUM_INTERVAL_DAYS = 36500;

// Predicted probability of recall `elapsedDays` after the review that
// produced `state`. 0 if never reviewed (stability <= 0).
float retrievability(const MemoryState &state, float elapsedDays,
                      const float weights[21] = DEFAULT_WEIGHTS);

// First-ever review of a card: derives initial stability/difficulty from a
// single rating (no prior state).
MemoryState firstReview(Rating rating, const float weights[21] = DEFAULT_WEIGHTS);

// Any later review. `elapsedDays` is the time since `prev`'s review (the one
// that produced `prev`) -- pass 0 for a same-day re-review, which takes a
// separate short-term update path.
MemoryState nextReview(const MemoryState &prev, Rating rating, float elapsedDays,
                        const float weights[21] = DEFAULT_WEIGHTS);

// Interval, in whole days (>= 1, capped at maximumIntervalDays), to schedule
// next so that predicted recall probability at due time is requestRetention.
uint32_t nextIntervalDays(float stability,
                           float requestRetention = DEFAULT_REQUEST_RETENTION,
                           uint32_t maximumIntervalDays = DEFAULT_MAXIMUM_INTERVAL_DAYS,
                           const float weights[21] = DEFAULT_WEIGHTS);

}  // namespace fsrs
