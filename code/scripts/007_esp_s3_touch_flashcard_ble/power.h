// Battery self-latch and the PWR button (long-press = shut down).
#pragma once

void powerBegin();    // latch battery power; call first thing in setup()
void handlePowerButton();   // poll from every loop iteration
