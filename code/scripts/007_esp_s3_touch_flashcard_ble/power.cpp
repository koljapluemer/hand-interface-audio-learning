#include <Arduino.h>
#include "config.h"
#include "power.h"
#include "ui.h"

void powerBegin() {
  pinMode(PIN_VBAT_PWR, OUTPUT);
  digitalWrite(PIN_VBAT_PWR, HIGH);
  pinMode(PIN_PWR_BTN, INPUT_PULLUP);
}

static void powerOff() {
  Serial.println("PWR long-press -> shutting down");
  uiRenderBye();
  digitalWrite(PIN_VBAT_PWR, LOW);
  while (true) delay(1000);
}

void handlePowerButton() {
  static bool down = false;
  static uint32_t downAt = 0;
  if (digitalRead(PIN_PWR_BTN) == LOW) {
    if (!down) { down = true; downAt = millis(); }
    else if (millis() - downAt >= PWR_LONGPRESS_MS) powerOff();
  } else {
    down = false;
  }
}
