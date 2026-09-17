#include "fsrs.h"

#include <math.h>

namespace fsrs {

const float DEFAULT_WEIGHTS[21] = {
    0.212f,  1.2931f, 2.3065f, 8.2956f, 6.4133f, 0.8334f, 3.0194f, 0.001f,
    1.8722f, 0.1666f, 0.796f,  1.4835f, 0.0614f, 0.2629f, 1.6483f, 0.6014f,
    1.8729f, 0.5425f, 0.0912f, 0.0658f, 0.1542f,
};

namespace {

constexpr float STABILITY_MIN = 0.001f;
constexpr float INITIAL_STABILITY_MAX = 100.0f;
constexpr float MIN_DIFFICULTY = 1.0f;
constexpr float MAX_DIFFICULTY = 10.0f;

float clampStability(float s) { return s < STABILITY_MIN ? STABILITY_MIN : s; }

float clampDifficulty(float d) {
  if (d < MIN_DIFFICULTY) return MIN_DIFFICULTY;
  if (d > MAX_DIFFICULTY) return MAX_DIFFICULTY;
  return d;
}

// decay/factor are derived from w[20]; py-fsrs recomputes them from
// `parameters` on every Scheduler construction rather than hardcoding them,
// so we do the same here in case a caller ever passes non-default weights.
float decayOf(const float w[21]) { return -w[20]; }
float factorOf(const float w[21]) {
  return powf(0.9f, 1.0f / decayOf(w)) - 1.0f;
}

float initialStability(Rating rating, const float w[21]) {
  return clampStability(w[(uint8_t)rating - 1]);
}

// clamp=false variant is needed internally by nextDifficulty's mean-reversion
// term (arg_1 in py-fsrs), which deliberately uses the *unclamped* value.
float initialDifficulty(Rating rating, bool clamp, const float w[21]) {
  float d = w[4] - expf(w[5] * ((uint8_t)rating - 1)) + 1.0f;
  return clamp ? clampDifficulty(d) : d;
}

float nextDifficulty(float difficulty, Rating rating, const float w[21]) {
  float arg1 = initialDifficulty(EASY, /*clamp=*/false, w);
  float deltaD = -(w[6] * ((uint8_t)rating - 3));
  float arg2 = difficulty + (10.0f - difficulty) * deltaD / 9.0f;
  float next = w[7] * arg1 + (1.0f - w[7]) * arg2;
  return clampDifficulty(next);
}

float shortTermStability(float stability, Rating rating, const float w[21]) {
  float inc = expf(w[17] * ((uint8_t)rating - 3 + w[18])) * powf(stability, -w[19]);
  if (rating == HARD || rating == GOOD || rating == EASY) {
    if (inc < 1.0f) inc = 1.0f;
  }
  return clampStability(stability * inc);
}

float nextForgetStability(float difficulty, float stability, float r, const float w[21]) {
  float longTerm = w[11] * powf(difficulty, -w[12]) *
                    (powf(stability + 1.0f, w[13]) - 1.0f) *
                    expf((1.0f - r) * w[14]);
  float shortTerm = stability / expf(w[17] * w[18]);
  return longTerm < shortTerm ? longTerm : shortTerm;
}

float nextRecallStability(float difficulty, float stability, float r, Rating rating,
                           const float w[21]) {
  float hardPenalty = (rating == HARD) ? w[15] : 1.0f;
  float easyBonus = (rating == EASY) ? w[16] : 1.0f;
  return stability * (1.0f + expf(w[8]) * (11.0f - difficulty) * powf(stability, -w[9]) *
                                  (expf((1.0f - r) * w[10]) - 1.0f) * hardPenalty * easyBonus);
}

float nextStability(float difficulty, float stability, float r, Rating rating,
                     const float w[21]) {
  float next = (rating == AGAIN) ? nextForgetStability(difficulty, stability, r, w)
                                  : nextRecallStability(difficulty, stability, r, rating, w);
  return clampStability(next);
}

}  // namespace

float retrievability(const MemoryState &state, float elapsedDays, const float w[21]) {
  if (state.stability <= 0.0f) return 0.0f;
  if (elapsedDays < 0.0f) elapsedDays = 0.0f;
  return powf(1.0f + factorOf(w) * elapsedDays / state.stability, decayOf(w));
}

MemoryState firstReview(Rating rating, const float w[21]) {
  MemoryState s;
  s.stability = initialStability(rating, w);
  s.difficulty = initialDifficulty(rating, /*clamp=*/true, w);
  return s;
}

MemoryState nextReview(const MemoryState &prev, Rating rating, float elapsedDays,
                        const float w[21]) {
  MemoryState next;
  if (elapsedDays < 1.0f) {
    next.stability = shortTermStability(prev.stability, rating, w);
  } else {
    float r = retrievability(prev, elapsedDays, w);
    next.stability = nextStability(prev.difficulty, prev.stability, r, rating, w);
  }
  next.difficulty = nextDifficulty(prev.difficulty, rating, w);
  return next;
}

uint32_t nextIntervalDays(float stability, float requestRetention,
                           uint32_t maximumIntervalDays, const float w[21]) {
  float factor = factorOf(w);
  float decay = decayOf(w);
  float days = (stability / factor) * (powf(requestRetention, 1.0f / decay) - 1.0f);

  int32_t rounded = (int32_t)lroundf(days);
  if (rounded < 1) rounded = 1;
  if ((uint32_t)rounded > maximumIntervalDays) rounded = (int32_t)maximumIntervalDays;
  return (uint32_t)rounded;
}

}  // namespace fsrs
