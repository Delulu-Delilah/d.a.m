/**
 * ST7735 160×80 — Material splash + dashboard only (T-Dongle).
 */

#include "axis_display.hpp"

#if defined(BOARD_T_DONGLE_S3)

#include "daydream_wireframe_data.hpp"
#include "lgfx_dongle.hpp"
#include <lgfx/v1/lgfx_fonts.hpp>
#include <cmath>

static LGFX_Dongle gDisplay;
static uint32_t lastDrawMs = 0;
static constexpr uint32_t kDrawIntervalMs = 40;
static constexpr int kW = 160;
static constexpr int kH = 80;

static constexpr uint16_t kBg = 0x1082;
static constexpr uint16_t kSurface = 0x18E3;
static constexpr uint16_t kPrimary = 0x2596;
static constexpr uint16_t kPrimaryContainer = 0x2965;
static constexpr uint16_t kOnSurface = 0xDEFB;
static constexpr uint16_t kOnSurfaceDim = 0xAD55;
static constexpr uint16_t kOutline = 0x4A69;
static constexpr uint16_t kActiveBtn = 0x4DDB;

static uint32_t bootMs = 0;
static bool splashDone = false;

static void applyLandscape160x80() {
  for (uint8_t r = 0; r < 4; ++r) {
    gDisplay.setRotation(r);
    if (gDisplay.width() == kW && gDisplay.height() == kH) {
      gDisplay.setRotation((r + 2) & 3);
      return;
    }
  }
  gDisplay.setRotation((1 + 2) & 3);
}

static const char *modeLabel(uint8_t m) {
  switch (m) {
  case 0:
    return "AIR MOUSE";
  case 1:
    return "TRACKPAD";
  case 2:
    return "MEDIA";
  case 3:
    return "D-PAD";
  default:
    return "?";
  }
}

static void fillMaterialBackground() { gDisplay.fillScreen(kBg); }

static void drawSplash() {
  fillMaterialBackground();
  gDisplay.fillRoundRect(6, 6, 148, 68, 10, kSurface);
  gDisplay.drawRoundRect(6, 6, 148, 68, 10, kPrimaryContainer);
  gDisplay.setFont(&fonts::Font2);
  gDisplay.setTextColor(kOnSurface);
  gDisplay.setCursor(18, 18);
  gDisplay.print("Daydream Air");
  gDisplay.setCursor(52, 34);
  gDisplay.print("Mouse");
  gDisplay.setTextColor(kPrimary);
  gDisplay.setCursor(58, 50);
  gDisplay.print("Dongle");
}

static void rotBasis(float roll, float pitch, float yaw, float ax[3], float ay[3],
                     float az[3]) {
  float cr = cosf(roll), sr = sinf(roll);
  float cp = cosf(pitch), sp = sinf(pitch);
  float cy = cosf(yaw), sy = sinf(yaw);
  ax[0] = cy * cp;
  ax[1] = sy * cp;
  ax[2] = -sp;
  ay[0] = cy * sp * sr - sy * cr;
  ay[1] = sy * sp * sr + cy * cr;
  ay[2] = cp * sr;
  az[0] = cy * sp * cr + sy * sr;
  az[1] = sy * sp * cr - cy * sr;
  az[2] = cp * cr;
}

static void localToWorld(const float ax[3], const float ay[3], const float az[3],
                         float lx, float ly, float lz, float o[3]) {
  o[0] = ax[0] * lx + ay[0] * ly + az[0] * lz;
  o[1] = ax[1] * lx + ay[1] * ly + az[1] * lz;
  o[2] = ax[2] * lx + ay[2] * ly + az[2] * lz;
}

static void screenProj(int ox, int oy, float scale, float vx, float vy, float vz,
                       int &sx, int &sy) {
  float px = vx - 0.42f * vz;
  float py = vy - 0.32f * vz;
  sx = ox + (int)(scale * px);
  sy = oy - (int)(scale * py);
}

static void drawControllerWireframe(const float ax[3], const float ay[3],
                                    const float az[3], int ox, int oy, float scale,
                                    uint16_t edgeColor) {
  int sx0, sy0, sx1, sy1;
  float w0[3], w1[3];

  for (int e = 0; e < kDaydreamEdgeCount; ++e) {
    uint16_t i0 = kDaydreamEdgeIdx[e * 2];
    uint16_t i1 = kDaydreamEdgeIdx[e * 2 + 1];
    localToWorld(ax, ay, az, kDaydreamVx[i0], kDaydreamVy[i0], kDaydreamVz[i0], w0);
    localToWorld(ax, ay, az, kDaydreamVx[i1], kDaydreamVy[i1], kDaydreamVz[i1], w1);
    screenProj(ox, oy, scale, w0[0], w0[1], w0[2], sx0, sy0);
    screenProj(ox, oy, scale, w1[0], w1[1], w1[2], sx1, sy1);
    gDisplay.drawLine(sx0, sy0, sx1, sy1, edgeColor);
  }
}

static void drawTrackpadDot(const DongleDisplayInput &in, int rx, int ry, int rw,
                            int rh) {
  gDisplay.drawRoundRect(rx, ry, rw, rh, 4, kOutline);
  bool touch = (in.xTouch > 0.02f || in.yTouch > 0.02f);
  if (touch) {
    int px = rx + 3 + (int)(in.xTouch * (float)(rw - 6));
    int py = ry + 3 + (int)(in.yTouch * (float)(rh - 6));
    gDisplay.fillCircle(px, py, 3, kActiveBtn);
    gDisplay.drawCircle(px, py, 3, kOnSurface);
  }
}

static void drawButtonPills(const DongleDisplayInput &in) {
  struct {
    const char *ch;
    bool on;
  } pills[] = {{"C", in.clickBtn}, {"H", in.homeBtn}, {"A", in.appBtn},
               {"-", in.volDownBtn}, {"+", in.volUpBtn}};
  const int py = 62;
  const int pw = 22;
  const int gap = 3;
  int x = 4;
  gDisplay.setFont(&fonts::Font0);
  for (int i = 0; i < 5; ++i) {
    if (pills[i].on) {
      gDisplay.fillRoundRect(x, py, pw, 16, 6, kPrimaryContainer);
      gDisplay.setTextColor(kOnSurface);
    } else {
      gDisplay.drawRoundRect(x, py, pw, 16, 6, kOutline);
      gDisplay.setTextColor(kOnSurfaceDim);
    }
    gDisplay.setCursor(x + 6, py + 4);
    gDisplay.print(pills[i].ch);
    x += pw + gap;
  }
}

static void drawDashboard(const DongleDisplayInput &in) {
  fillMaterialBackground();

  gDisplay.fillRoundRect(4, 4, 112, 16, 6, kSurface);
  gDisplay.drawRoundRect(4, 4, 112, 16, 6, kOutline);
  gDisplay.setFont(&fonts::Font0);
  gDisplay.setTextColor(kOnSurface);
  gDisplay.setCursor(10, 8);
  gDisplay.print(modeLabel(in.deviceMode));

  gDisplay.setTextColor(in.connected ? kActiveBtn : kOnSurfaceDim);
  gDisplay.setCursor(122, 8);
  if (!in.connected) {
    if (in.scanning)
      gDisplay.print("SCAN");
    else
      gDisplay.print("---");
  } else {
    gDisplay.print("OK");
  }

  float ax[3], ay[3], az[3];
  if (!in.connected) {
    ax[0] = 1.f;
    ax[1] = 0.f;
    ax[2] = 0.f;
    ay[0] = 0.f;
    ay[1] = 1.f;
    ay[2] = 0.f;
    az[0] = 0.f;
    az[1] = 0.f;
    az[2] = 1.f;
  } else {
    rotBasis(in.xOri, in.yOri, in.zOri, ax, ay, az);
  }

  uint16_t wf = kOnSurfaceDim;
  if (in.clickBtn)
    wf = kPrimary;
  // Full-width centered wand (drawn first); square trackpad overlays the top-right.
  drawControllerWireframe(ax, ay, az, 80, 40, 52.f, wf);

  drawTrackpadDot(in, 118, 22, 38, 38);
  drawButtonPills(in);
}

void axisDisplayInit() {
  if (!gDisplay.init()) {
    Serial.println("[AXIS] LGFX init failed");
    return;
  }
  applyLandscape160x80();
  bootMs = millis();
  splashDone = false;
  Serial.printf("[AXIS] Display %d×%d\n", gDisplay.width(), gDisplay.height());
}

void axisDisplayTick(const DongleDisplayInput &in) {
  uint32_t now = millis();
  if (now - lastDrawMs < kDrawIntervalMs)
    return;
  lastDrawMs = now;

  if (!splashDone) {
    if (now - bootMs < 2500) {
      drawSplash();
      return;
    }
    splashDone = true;
  }

  drawDashboard(in);
}

#else

void axisDisplayInit() {}

void axisDisplayTick(const DongleDisplayInput &) {}

#endif
