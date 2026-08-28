/*
 * 003_esp_c3_button_connection_test
 * ---------------------------------
 * Bring-up / wiring test for an ESP32-C3 dev board with the small onboard
 * SSD1306 OLED and THREE momentary push buttons on GPIO 3, 4 and 5.
 *
 * What it does:
 *   - Configures the three button pins as INPUT_PULLUP (wire each button
 *     between its GPIO and GND, no external resistor needed).
 *   - Continuously shows the live state (UP / DOWN) and a press counter for
 *     each button, both on the OLED and on the serial monitor (115200 baud).
 *   - A button is "connected correctly" if its line reads UP (HIGH) when
 *     released and DOWN (LOW) only while you hold it, and its counter goes up
 *     by exactly one per press.
 *
 * Reading the results:
 *   - Counter never moves / always UP .... button not reaching the pin (open
 *     wire, wrong pin, cold solder joint).
 *   - Line stuck DOWN / counter runs away .. button shorted to GND, or the pin
 *     is also being driven by something else.
 *   - Two counters move on one press ....... buttons share a wire / bridged pads.
 *
 * !!! PIN 5 CONFLICT WARNING !!!
 *   On the boards used elsewhere in this repo the onboard OLED talks I2C on
 *   SDA = GPIO5, SCL = GPIO6 (see 004_esp_c3_better_flashcard). GPIO5 is then
 *   NOT free for a button. If your board is one of those, either:
 *     (a) move the third button to another free pin (e.g. GPIO10) and update
 *         BTN_PINS below, or
 *     (b) set USE_DISPLAY to 0 and run this as a serial-only test.
 *   If your board's OLED is on different pins, adjust OLED_SDA / OLED_SCL and
 *   ignore this warning.
 */

#define USE_DISPLAY 1          // set to 0 for a serial-only test (no OLED)

#include <Arduino.h>

#if USE_DISPLAY
  #include <U8g2lib.h>
  #include <Wire.h>

  #define OLED_SDA 5            // onboard OLED I2C data  (conflicts with button on 5!)
  #define OLED_SCL 6            // onboard OLED I2C clock

  // 72x40 EastRising SSD1306, same part as the other scripts in this repo.
  U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
#endif

// --- Buttons under test -----------------------------------------------------
const uint8_t BTN_PINS[] = { 3, 4, 5 };
const uint8_t NUM_BTNS   = sizeof(BTN_PINS) / sizeof(BTN_PINS[0]);

const uint16_t DEBOUNCE_MS = 25;

uint8_t  stableState[NUM_BTNS];      // debounced level (HIGH = released)
uint8_t  lastReading[NUM_BTNS];      // last raw reading
uint32_t lastChangeMs[NUM_BTNS];     // when the raw reading last changed
uint32_t pressCount[NUM_BTNS];       // confirmed presses since boot

uint32_t lastHeartbeatMs = 0;

void printSerialStatus() {
  Serial.print("buttons  ");
  for (uint8_t i = 0; i < NUM_BTNS; i++) {
    Serial.print('P');
    Serial.print(BTN_PINS[i]);
    Serial.print(':');
    Serial.print(stableState[i] == LOW ? "DOWN" : "UP  ");
    Serial.print(" (");
    Serial.print(pressCount[i]);
    Serial.print(")   ");
  }
  Serial.println();
}

#if USE_DISPLAY
void drawDisplay() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x8_tr);

  char line[24];
  for (uint8_t i = 0; i < NUM_BTNS; i++) {
    snprintf(line, sizeof(line), "P%d %s %lu",
             BTN_PINS[i],
             stableState[i] == LOW ? "DN" : "UP",
             (unsigned long)pressCount[i]);
    u8g2.drawStr(0, 7 + i * 11, line);
  }
  u8g2.sendBuffer();
}
#endif

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("ESP32-C3 button connection test (pins 3, 4, 5)");
  Serial.println("Press each button in turn; watch the counters.");

  for (uint8_t i = 0; i < NUM_BTNS; i++) {
    pinMode(BTN_PINS[i], INPUT_PULLUP);
    stableState[i] = HIGH;
    lastReading[i] = HIGH;
    lastChangeMs[i] = 0;
    pressCount[i] = 0;
  }

#if USE_DISPLAY
  Wire.begin(OLED_SDA, OLED_SCL);
  u8g2.begin();
  u8g2.setContrast(255);
  drawDisplay();
#endif
}

void loop() {
  uint32_t now = millis();
  bool changed = false;

  for (uint8_t i = 0; i < NUM_BTNS; i++) {
    uint8_t raw = digitalRead(BTN_PINS[i]);

    if (raw != lastReading[i]) {
      lastReading[i] = raw;
      lastChangeMs[i] = now;
    }

    // Accept the reading once it has been stable long enough.
    if ((now - lastChangeMs[i]) >= DEBOUNCE_MS && raw != stableState[i]) {
      stableState[i] = raw;
      changed = true;
      if (raw == LOW) {                 // HIGH -> LOW = a fresh press
        pressCount[i]++;
        Serial.print("PRESS  P");
        Serial.print(BTN_PINS[i]);
        Serial.print("  count=");
        Serial.println(pressCount[i]);
      }
    }
  }

  if (changed || (now - lastHeartbeatMs) >= 1000) {
    lastHeartbeatMs = now;
    printSerialStatus();
#if USE_DISPLAY
    drawDisplay();
#endif
  }

  delay(5);
}
