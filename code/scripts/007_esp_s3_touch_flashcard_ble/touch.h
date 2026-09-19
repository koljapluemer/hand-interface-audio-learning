// FT6336 capacitive touch (I2C), polled.
#pragma once

void touchBegin();   // reset the controller and start I2C

// True once per fresh finger-down, with the touch-down point. One shared
// edge detector, fine because only the main loop polls.
bool pollTapDown(int &x, int &y);

inline bool inRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}
