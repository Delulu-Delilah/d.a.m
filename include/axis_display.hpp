#pragma once

#include <Arduino.h>

/** T-Dongle ST7735: one frame of UI state from main loop. */
struct DongleDisplayInput {
  bool connected;
  bool scanning;
  uint8_t deviceMode; // 0=AIR_MOUSE .. 3=D-PAD (matches DeviceMode in main.cpp)
  float xOri, yOri, zOri;
  float xGyro, yGyro, zGyro;
  bool clickBtn, homeBtn, appBtn, volDownBtn, volUpBtn;
  float xTouch, yTouch;
};

void axisDisplayInit();
void axisDisplayTick(const DongleDisplayInput &in);
