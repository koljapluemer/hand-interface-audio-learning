#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include "touch.h"

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

void touchBegin() {
  pinMode(PIN_TP_INT, INPUT_PULLUP);
  ft6336Reset();
  Wire.begin(PIN_TP_SDA, PIN_TP_SCL, 400000);
}

bool pollTapDown(int &x, int &y) {
  static bool was = false;
  bool now = readTouch(x, y);
  bool down = now && !was;
  was = now;
  return down;
}
