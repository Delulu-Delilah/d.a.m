/**
 * Daydream Controller → USB Air Mouse + Media Remote
 *
 * Firmware for Seeed XIAO ESP32-S3 Sense
 * Connects to up to TWO Google Daydream controllers over BLE and
 * exposes USB HID Mouse + Consumer Control for wireless pointing
 * and media control.
 *
 * Multi-Controller:
 *   - Scans and connects to up to 2 Daydream controllers simultaneously
 *   - Boot button (GPIO 0) or App+VolDown combo switches active controller
 *   - LED feedback: breathing = scanning, solid = connected
 *
 * Modes (cycled by Home/Daydream button on active controller):
 *   - AIR MOUSE:  Orientation + accel fusion → mouse movement
 *                 Trackpad → scroll wheel
 *   - TRACKPAD:   Touchpad deltas → mouse movement
 *   - MEDIA:      Trackpad gestures → media control
 *
 * Sensitivity:  Home + Vol Up = increase, Home + Vol Down = decrease
 *               (auto-saved to flash, persists across reboots)
 *
 * Home (short press): Cycle modes
 * Home (hold 1s):     Recenter orientation (air mouse mode)
 *
 * Supports both old and new Daydream controller firmware via
 * BLE bonding + explicit CCCD descriptor writes.
 */

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <USB.h>
#include <USBHIDConsumerControl.h>
#include <USBHIDMouse.h>

// ─── Board Selection ────────────────────────────────────────────────────────
// Each board config defines: BOARD_NAME, BOARD_LED_PIN, BOARD_BOOT_PIN,
// BOARD_LED_INVERTED. To add a new board, create a header in boards/ and
// add a -DBOARD_xxx build flag in platformio.ini.

#if defined(BOARD_XIAO_ESP32S3)
#include "../boards/xiao_esp32s3.h"
// ── Add new boards here ──
// #elif defined(BOARD_ESP32S3_DEVKIT)
//   #include "../boards/esp32s3_devkit.h"
#else
// Default: XIAO ESP32-S3
#include "../boards/xiao_esp32s3.h"
#endif

// Resolve board config into internal constants
static const int LED_PIN = BOARD_LED_PIN;
static const int BOOT_BTN_PIN = BOARD_BOOT_PIN;
static const bool LED_INVERT = BOARD_LED_INVERTED;

// LED polarity helpers (active-low vs active-high)
#define LED_ON() digitalWrite(LED_PIN, LED_INVERT ? LOW : HIGH)
#define LED_OFF() digitalWrite(LED_PIN, LED_INVERT ? HIGH : LOW)

// ─── Device Modes ───────────────────────────────────────────────────────────

enum DeviceMode { MODE_AIR_MOUSE, MODE_TRACKPAD, MODE_MEDIA };

static const char *modeNames[] = {"AIR MOUSE", "TRACKPAD", "MEDIA"};
static DeviceMode currentMode = MODE_AIR_MOUSE;

// ─── Configuration (defaults, overridden by saved prefs) ────────────────────

static float airMouseSensitivity = 1800.0f;
static float trackpadSensitivity = 6.0f;
static float scrollSensitivity = 3.0f;
static const float SMOOTH_ALPHA = 0.45f;

static const float AIR_MOUSE_DEADZONE = 0.003f;
static const float SCROLL_DEADZONE = 0.008f;
static const float SWIPE_THRESHOLD = 0.25f;

static const float SENSITIVITY_STEP = 0.15f; // 15% per adjustment

static const int BLE_SCAN_DURATION_SEC = 10;
static const int RECONNECT_DELAY_MS = 3000;
static const int DISCOVERY_INTERVAL_MS = 15000;
static const unsigned long HOME_LONG_PRESS_MS = 1000;
static const unsigned long AUTO_SLEEP_MS = 120000; // 2 min

// ─── Multi-Controller ───────────────────────────────────────────────────────

static const int MAX_CONTROLLERS = 2;

// ─── Daydream BLE Protocol ──────────────────────────────────────────────────

static NimBLEUUID SERVICE_UUID("0000fe55-0000-1000-8000-00805f9b34fb");
static NimBLEUUID CHAR_UUID("00000001-1000-1000-8000-00805f9b34fb");
static NimBLEUUID CCCD_UUID("00002902-0000-1000-8000-00805f9b34fb");

static const uint8_t BTN_CLICK = 0x01;
static const uint8_t BTN_HOME = 0x02;
static const uint8_t BTN_APP = 0x04;
static const uint8_t BTN_VOL_DOWN = 0x08;
static const uint8_t BTN_VOL_UP = 0x10;

// ─── Parsed Controller State ────────────────────────────────────────────────

struct DaydreamState {
  float xOri, yOri, zOri;
  float xAcc, yAcc, zAcc;
  float xGyro, yGyro, zGyro;
  float xTouch, yTouch;
  bool clickBtn, homeBtn, appBtn, volDownBtn, volUpBtn;
};

// ─── Controller Slot ────────────────────────────────────────────────────────

struct ControllerSlot {
  NimBLEClient *client = nullptr;
  NimBLEAdvertisedDevice *device = nullptr;
  bool connected = false;
  bool doConnect = false;
  bool notificationsWorking = false;
  unsigned long connectTime = 0;
  unsigned long notifyCount = 0;
  unsigned long lastNotifyTime = 0;

  DaydreamState current = {};
  DaydreamState previous = {};
  bool stateUpdated = false;

  bool wasTouching = false;
  float prevTouchX = 0, prevTouchY = 0;
  float lastTouchX = 0;
  float swipeStartX = 0;
  bool swipeActive = false;
  bool hasRefOrientation = false;
  float refXOri = 0, refYOri = 0;
  float smoothDx = 0, smoothDy = 0;

  bool prevClickBtn = false;
  bool prevHomeBtn = false;
  bool prevAppBtn = false;
  bool prevVolDownBtn = false;
  bool prevVolUpBtn = false;

  unsigned long homePressSince = 0;
  bool homeLongFired = false;

  NimBLEAddress address;
  bool hasAddress = false;
};

// ─── Globals ────────────────────────────────────────────────────────────────

USBHIDMouse Mouse;
USBHIDConsumerControl ConsumerControl;
Preferences prefs;

static ControllerSlot slots[MAX_CONTROLLERS];
static int activeSlot = 0;
static bool scanning = false;
static NimBLEAddress knownAddresses[MAX_CONTROLLERS];
static int knownAddressCount = 0;

static volatile bool bootBtnPressed = false;
static unsigned long lastBootBtnTime = 0;

// LED state machine
enum LEDState {
  LED_STATE_OFF,
  LED_STATE_BREATHING,
  LED_STATE_SOLID,
  LED_STATE_FLASH
};
static LEDState ledState = LED_STATE_OFF;
static unsigned long ledLastUpdate = 0;
static float ledBreathPhase = 0;

// Auto-sleep
static unsigned long lastControllerActivity = 0;
static bool sleepMode = false;

// ─── Preferences ────────────────────────────────────────────────────────────

void loadPreferences() {
  prefs.begin("ddmouse", true); // read-only
  airMouseSensitivity = prefs.getFloat("airSens", 1800.0f);
  trackpadSensitivity = prefs.getFloat("tpadSens", 6.0f);
  scrollSensitivity = prefs.getFloat("scrollSens", 8.0f);
  prefs.end();
  Serial.printf("[PREF] Loaded: air=%.0f tpad=%.1f scroll=%.1f\n",
                airMouseSensitivity, trackpadSensitivity, scrollSensitivity);
}

void savePreferences() {
  prefs.begin("ddmouse", false);
  prefs.putFloat("airSens", airMouseSensitivity);
  prefs.putFloat("tpadSens", trackpadSensitivity);
  prefs.putFloat("scrollSens", scrollSensitivity);
  prefs.end();
  Serial.printf("[PREF] Saved: air=%.0f tpad=%.1f scroll=%.1f\n",
                airMouseSensitivity, trackpadSensitivity, scrollSensitivity);
}

// ─── LED Helpers ────────────────────────────────────────────────────────────

void ledFlash(int count, int onMs = 100, int offMs = 100) {
  for (int i = 0; i < count; i++) {
    LED_ON();
    delay(onMs);
    LED_OFF();
    delay(offMs);
  }
}

// Non-blocking breathing LED (call from loop)
void ledUpdateBreathing() {
  if (ledState != LED_STATE_BREATHING)
    return;
  unsigned long now = millis();
  if (now - ledLastUpdate < 16)
    return; // ~60fps
  ledLastUpdate = now;

  ledBreathPhase += 0.04f;
  if (ledBreathPhase > 2.0f * PI)
    ledBreathPhase -= 2.0f * PI;

  // Sine wave breathing: 0→255→0
  float brightness = (sinf(ledBreathPhase) + 1.0f) * 0.5f;
  int pwm = (int)(brightness * 255.0f);
  analogWrite(LED_PIN, LED_INVERT ? (255 - pwm) : pwm);
}

void ledSetBreathing() {
  ledState = LED_STATE_BREATHING;
  ledBreathPhase = 0;
}

void ledSetSolid() {
  ledState = LED_STATE_SOLID;
  LED_ON();
}

void ledSetOff() {
  ledState = LED_STATE_OFF;
  LED_OFF();
}

void ledIndicateMode(DeviceMode mode) {
  switch (mode) {
  case MODE_AIR_MOUSE:
    ledFlash(1, 200, 0);
    break;
  case MODE_TRACKPAD:
    ledFlash(2, 100, 100);
    break;
  case MODE_MEDIA:
    ledFlash(3, 60, 60);
    break;
  }
  // Restore solid if connected
  bool anyConnected = false;
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    if (slots[i].connected) {
      anyConnected = true;
      break;
    }
  }
  if (anyConnected)
    ledSetSolid();
  else
    ledSetBreathing();
}

void ledIndicateSlot(int slot) {
  ledFlash(slot + 1, 150, 150);
  ledSetSolid();
}

void ledIndicateSensitivity(bool increase) {
  // Quick double-flash for up, triple for down
  ledFlash(increase ? 2 : 3, 40, 40);
  ledSetSolid();
}

void ledIndicateBattery(int level) {
  // level 0-3: flash count indicates battery (0=empty, 3=full)
  int flashes = constrain(level, 1, 4);
  delay(300);
  ledFlash(flashes, 200, 200);
  delay(300);
}

// ─── Boot Button ISR ────────────────────────────────────────────────────────

void IRAM_ATTR bootBtnISR() { bootBtnPressed = true; }

// ─── Packet Parser ──────────────────────────────────────────────────────────

static int16_t signExtend13(int raw) {
  if (raw & 0x1000)
    raw |= 0xE000;
  return (int16_t)raw;
}

void parsePacket(const uint8_t *data, size_t len, DaydreamState &state) {
  if (len < 20)
    return;

  int rawXOri = ((data[1] & 0x03) << 11) | ((data[2] & 0xFF) << 3) |
                ((data[3] & 0xE0) >> 5);
  state.xOri = signExtend13(rawXOri) * (2.0f * PI / 4095.0f);

  int rawYOri = ((data[3] & 0x1F) << 8) | (data[4] & 0xFF);
  state.yOri = signExtend13(rawYOri) * (2.0f * PI / 4095.0f);

  int rawZOri = ((data[5] & 0xFF) << 5) | ((data[6] & 0xF8) >> 3);
  state.zOri = signExtend13(rawZOri) * (2.0f * PI / 4095.0f);

  int rawXAcc = ((data[6] & 0x07) << 10) | ((data[7] & 0xFF) << 2) |
                ((data[8] & 0xC0) >> 6);
  state.xAcc = signExtend13(rawXAcc) * (8.0f * 9.8f / 4095.0f);

  int rawYAcc = ((data[8] & 0x3F) << 7) | ((data[9] & 0xFE) >> 1);
  state.yAcc = signExtend13(rawYAcc) * (8.0f * 9.8f / 4095.0f);

  int rawZAcc = ((data[9] & 0x01) << 12) | ((data[10] & 0xFF) << 4) |
                ((data[11] & 0xF0) >> 4);
  state.zAcc = signExtend13(rawZAcc) * (8.0f * 9.8f / 4095.0f);

  int rawXGyro = ((data[11] & 0x0F) << 9) | ((data[12] & 0xFF) << 1) |
                 ((data[13] & 0x80) >> 7);
  state.xGyro = signExtend13(rawXGyro) * (2048.0f / 180.0f * PI / 4095.0f);

  int rawYGyro = ((data[13] & 0x7F) << 6) | ((data[14] & 0xFC) >> 2);
  state.yGyro = signExtend13(rawYGyro) * (2048.0f / 180.0f * PI / 4095.0f);

  int rawZGyro = ((data[14] & 0x03) << 11) | ((data[15] & 0xFF) << 3) |
                 ((data[16] & 0xE0) >> 5);
  state.zGyro = signExtend13(rawZGyro) * (2048.0f / 180.0f * PI / 4095.0f);

  state.xTouch =
      (float)(((data[16] & 0x1F) << 3) | ((data[17] & 0xE0) >> 5)) / 255.0f;
  state.yTouch =
      (float)(((data[17] & 0x1F) << 3) | ((data[18] & 0xE0) >> 5)) / 255.0f;

  uint8_t btns = data[18];
  state.clickBtn = (btns & BTN_CLICK) != 0;
  state.homeBtn = (btns & BTN_HOME) != 0;
  state.appBtn = (btns & BTN_APP) != 0;
  state.volDownBtn = (btns & BTN_VOL_DOWN) != 0;
  state.volUpBtn = (btns & BTN_VOL_UP) != 0;
}

// ─── BLE Notification Callbacks ─────────────────────────────────────────────

void notifyCallbackSlot0(NimBLERemoteCharacteristic *pChar, uint8_t *data,
                         size_t length, bool isNotify) {
  ControllerSlot &s = slots[0];
  if (length >= 20) {
    s.previous = s.current;
    parsePacket(data, length, s.current);
    s.stateUpdated = true;
    s.lastNotifyTime = millis();
    s.notifyCount++;
    lastControllerActivity = millis();
    if (!s.notificationsWorking) {
      s.notificationsWorking = true;
      Serial.printf("[SLOT 0] ✓ First packet (%lu ms)\n",
                    millis() - s.connectTime);
    }
  }
}

void notifyCallbackSlot1(NimBLERemoteCharacteristic *pChar, uint8_t *data,
                         size_t length, bool isNotify) {
  ControllerSlot &s = slots[1];
  if (length >= 20) {
    s.previous = s.current;
    parsePacket(data, length, s.current);
    s.stateUpdated = true;
    s.lastNotifyTime = millis();
    s.notifyCount++;
    lastControllerActivity = millis();
    if (!s.notificationsWorking) {
      s.notificationsWorking = true;
      Serial.printf("[SLOT 1] ✓ First packet (%lu ms)\n",
                    millis() - s.connectTime);
    }
  }
}

typedef void (*NotifyCallback)(NimBLERemoteCharacteristic *, uint8_t *, size_t,
                               bool);
static NotifyCallback slotCallbacks[MAX_CONTROLLERS] = {notifyCallbackSlot0,
                                                        notifyCallbackSlot1};

// ─── BLE Client Callbacks ───────────────────────────────────────────────────

class ClientCallbacks : public NimBLEClientCallbacks {
public:
  int slotIndex;
  ClientCallbacks(int idx) : slotIndex(idx) {}

  void onConnect(NimBLEClient *pClient) override {
    ControllerSlot &s = slots[slotIndex];
    s.connected = true;
    s.notificationsWorking = false;
    s.notifyCount = 0;
    s.connectTime = millis();
    lastControllerActivity = millis();
    Serial.printf("[SLOT %d] Connected!\n", slotIndex);
    ledSetSolid();
  }

  void onDisconnect(NimBLEClient *pClient) override {
    ControllerSlot &s = slots[slotIndex];
    s.connected = false;
    s.notificationsWorking = false;
    s.hasRefOrientation = false;
    s.wasTouching = false;
    s.swipeActive = false;
    s.smoothDx = 0;
    s.smoothDy = 0;
    Serial.printf("[SLOT %d] Disconnected (%lu pkts)\n", slotIndex,
                  s.notifyCount);
    // Check if any controller still connected
    bool anyConnected = false;
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
      if (slots[i].connected) {
        anyConnected = true;
        break;
      }
    }
    if (!anyConnected)
      ledSetBreathing();
    else
      ledFlash(3, 50, 50);
  }

  uint32_t onPassKeyRequest() override { return 0; }
  bool onConfirmPIN(uint32_t pass_key) override { return true; }

  void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
    if (desc->sec_state.encrypted) {
      Serial.printf("[SLOT %d] ✓ Encrypted (bonded=%s)\n", slotIndex,
                    desc->sec_state.bonded ? "yes" : "no");
    }
  }
};

static ClientCallbacks *clientCallbacks[MAX_CONTROLLERS] = {nullptr, nullptr};

// ─── BLE Scan Callbacks ─────────────────────────────────────────────────────

bool isAddressKnown(const NimBLEAddress &addr) {
  for (int i = 0; i < knownAddressCount; i++) {
    if (knownAddresses[i] == addr)
      return true;
  }
  return false;
}

int findFreeSlot() {
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    if (!slots[i].connected && !slots[i].doConnect && !slots[i].hasAddress)
      return i;
  }
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    if (!slots[i].connected && !slots[i].doConnect)
      return i;
  }
  return -1;
}

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice *advertisedDevice) override {
    if (advertisedDevice->isAdvertisingService(SERVICE_UUID)) {
      NimBLEAddress addr = advertisedDevice->getAddress();
      if (isAddressKnown(addr))
        return;

      int slot = findFreeSlot();
      if (slot < 0) {
        NimBLEDevice::getScan()->stop();
        scanning = false;
        return;
      }

      Serial.printf("[SLOT %d] >>> Daydream found: %s\n", slot,
                    addr.toString().c_str());

      slots[slot].device = advertisedDevice;
      slots[slot].doConnect = true;
      slots[slot].address = addr;
      slots[slot].hasAddress = true;

      if (knownAddressCount < MAX_CONTROLLERS) {
        knownAddresses[knownAddressCount++] = addr;
      }

      int connectedOrPending = 0;
      for (int i = 0; i < MAX_CONTROLLERS; i++) {
        if (slots[i].connected || slots[i].doConnect)
          connectedOrPending++;
      }
      if (connectedOrPending >= MAX_CONTROLLERS) {
        NimBLEDevice::getScan()->stop();
        scanning = false;
      }
    }
  }
};

static ScanCallbacks scanCB;

// ─── BLE Connect ────────────────────────────────────────────────────────────

bool connectToController(int slotIdx) {
  ControllerSlot &s = slots[slotIdx];
  if (!s.device)
    return false;

  Serial.printf("[SLOT %d] Connecting...\n", slotIdx);

  if (!s.client) {
    s.client = NimBLEDevice::createClient();
    if (!clientCallbacks[slotIdx])
      clientCallbacks[slotIdx] = new ClientCallbacks(slotIdx);
    s.client->setClientCallbacks(clientCallbacks[slotIdx]);
    s.client->setConnectionParams(12, 12, 0, 150);
    s.client->setConnectTimeout(10);
  }

  if (!s.client->connect(s.device)) {
    Serial.printf("[SLOT %d] Connection failed!\n", slotIdx);
    return false;
  }

  NimBLERemoteService *pService = s.client->getService(SERVICE_UUID);
  if (!pService) {
    s.client->disconnect();
    return false;
  }

  NimBLERemoteCharacteristic *pChar = pService->getCharacteristic(CHAR_UUID);
  if (!pChar) {
    s.client->disconnect();
    return false;
  }

  Serial.printf("[SLOT %d] Requesting bonding...\n", slotIdx);
  s.client->secureConnection();
  delay(500);

  if (pChar->canNotify()) {
    if (!pChar->subscribe(true, slotCallbacks[slotIdx])) {
      NimBLERemoteDescriptor *cccd = pChar->getDescriptor(CCCD_UUID);
      if (cccd) {
        uint8_t enableNotify[] = {0x01, 0x00};
        cccd->writeValue(enableNotify, 2, true);
        pChar->subscribe(true, slotCallbacks[slotIdx]);
      }
    }
  } else {
    s.client->disconnect();
    return false;
  }

  unsigned long waitStart = millis();
  while (!s.notificationsWorking && millis() - waitStart < 3000)
    delay(50);

  if (!s.notificationsWorking) {
    pChar->unsubscribe();
    delay(200);
    NimBLERemoteDescriptor *cccd = pChar->getDescriptor(CCCD_UUID);
    if (cccd) {
      uint8_t enableNotify[] = {0x01, 0x00};
      cccd->writeValue(enableNotify, 2, true);
    }
    pChar->subscribe(true, slotCallbacks[slotIdx]);
    waitStart = millis();
    while (!s.notificationsWorking && millis() - waitStart < 3000)
      delay(50);
  }

  if (s.notificationsWorking) {
    Serial.printf("[SLOT %d] ✓ Data stream active!\n", slotIdx);
  } else {
    Serial.printf("[SLOT %d] ✗ No data\n", slotIdx);
  }

  return true;
}

// ─── BLE Scan Start ─────────────────────────────────────────────────────────

void onScanComplete(NimBLEScanResults results) {
  scanning = false;
  Serial.printf("[BLE] Scan complete (%d results)\n", results.getCount());
}

void startScan() {
  if (scanning || sleepMode)
    return;

  int needed = 0;
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    if (!slots[i].connected && !slots[i].doConnect)
      needed++;
  }
  if (needed == 0)
    return;

  Serial.printf("[BLE] Scanning (%d slot%s)...\n", needed,
                needed > 1 ? "s" : "");
  scanning = true;
  ledSetBreathing();

  NimBLEScan *pScan = NimBLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(&scanCB);
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);
  pScan->start(BLE_SCAN_DURATION_SEC, onScanComplete);
}

// ─── Air Mouse Movement ─────────────────────────────────────────────────────

void processAirMouse(ControllerSlot &s) {
  if (!s.hasRefOrientation) {
    s.refXOri = s.current.xOri;
    s.refYOri = s.current.yOri;
    s.hasRefOrientation = true;
    return;
  }

  float deltaYaw = s.current.yOri - s.previous.yOri;
  float deltaPitch = s.current.xOri - s.previous.xOri;

  if (fabsf(deltaYaw) > 1.0f)
    deltaYaw = 0;
  if (fabsf(deltaPitch) > 1.0f)
    deltaPitch = 0;
  if (fabsf(deltaYaw) < AIR_MOUSE_DEADZONE)
    deltaYaw = 0;
  if (fabsf(deltaPitch) < AIR_MOUSE_DEADZONE)
    deltaPitch = 0;

  float rawDx = -deltaYaw * airMouseSensitivity;
  float rawDy = -deltaPitch * airMouseSensitivity;

  s.smoothDx = SMOOTH_ALPHA * rawDx + (1.0f - SMOOTH_ALPHA) * s.smoothDx;
  s.smoothDy = SMOOTH_ALPHA * rawDy + (1.0f - SMOOTH_ALPHA) * s.smoothDy;

  int8_t dx = (int8_t)constrain((int)s.smoothDx, -127, 127);
  int8_t dy = (int8_t)constrain((int)s.smoothDy, -127, 127);

  int8_t scroll = 0;
  bool isTouching = (s.current.xTouch > 0.01f || s.current.yTouch > 0.01f);

  if (isTouching) {
    if (s.wasTouching) {
      float deltaScrollY = (s.current.yTouch - s.prevTouchY);
      if (fabsf(deltaScrollY) > SCROLL_DEADZONE) {
        scroll = (int8_t)constrain(
            (int)(-deltaScrollY * 255.0f * scrollSensitivity), -127, 127);
      }
    }
    s.prevTouchX = s.current.xTouch;
    s.prevTouchY = s.current.yTouch;
    s.wasTouching = true;
  } else {
    s.wasTouching = false;
  }

  if (dx != 0 || dy != 0 || scroll != 0)
    Mouse.move(dx, dy, scroll);
}

// ─── Trackpad Mode ──────────────────────────────────────────────────────────

void processTrackpad(ControllerSlot &s) {
  bool isTouching = (s.current.xTouch > 0.01f || s.current.yTouch > 0.01f);

  if (isTouching) {
    if (s.wasTouching) {
      float deltaX = (s.current.xTouch - s.prevTouchX) * 255.0f;
      float deltaY = (s.current.yTouch - s.prevTouchY) * 255.0f;

      float fx = deltaX * trackpadSensitivity;
      float fy = deltaY * trackpadSensitivity;

      int8_t dx = (int8_t)constrain((int)fx, -127, 127);
      int8_t dy = (int8_t)constrain((int)fy, -127, 127);

      if (dx != 0 || dy != 0)
        Mouse.move(dx, dy, 0);
    }
    s.prevTouchX = s.current.xTouch;
    s.prevTouchY = s.current.yTouch;
    s.wasTouching = true;
  } else {
    s.wasTouching = false;
  }
}

// ─── Media Mode Gestures ────────────────────────────────────────────────────

void processMediaGestures(ControllerSlot &s) {
  bool isTouching = (s.current.xTouch > 0.01f || s.current.yTouch > 0.01f);

  if (isTouching) {
    s.lastTouchX = s.current.xTouch;
    if (!s.swipeActive) {
      s.swipeStartX = s.current.xTouch;
      s.swipeActive = true;
    }
  } else if (s.swipeActive) {
    float swipeDelta = s.lastTouchX - s.swipeStartX;

    if (swipeDelta > SWIPE_THRESHOLD) {
      ConsumerControl.press(CONSUMER_CONTROL_SCAN_NEXT);
      ConsumerControl.release();
    } else if (swipeDelta < -SWIPE_THRESHOLD) {
      ConsumerControl.press(CONSUMER_CONTROL_SCAN_PREVIOUS);
      ConsumerControl.release();
    }
    s.swipeActive = false;
  }
}

// ─── Movement Dispatcher ────────────────────────────────────────────────────

void processMovement(ControllerSlot &s) {
  if (!s.stateUpdated)
    return;
  s.stateUpdated = false;

  switch (currentMode) {
  case MODE_AIR_MOUSE:
    processAirMouse(s);
    break;
  case MODE_TRACKPAD:
    processTrackpad(s);
    break;
  case MODE_MEDIA:
    processMediaGestures(s);
    break;
  }
}

// ─── Controller Switch Combo ────────────────────────────────────────────────

static bool switchComboFired = false;

void checkSwitchCombo(ControllerSlot &s) {
  bool comboHeld = s.current.appBtn && s.current.volDownBtn;
  bool comboJustPressed = comboHeld && (!s.prevAppBtn || !s.prevVolDownBtn);

  if (comboJustPressed && !switchComboFired) {
    switchComboFired = true;
    int newSlot = (activeSlot + 1) % MAX_CONTROLLERS;
    if (slots[newSlot].connected) {
      activeSlot = newSlot;
      Serial.printf("[SWITCH] Active: Slot %d\n", activeSlot);
      ledIndicateSlot(activeSlot);
    } else {
      ledFlash(5, 30, 30);
    }
  }
  if (!s.current.appBtn && !s.current.volDownBtn) {
    switchComboFired = false;
  }
}

// ─── Sensitivity Adjustment ─────────────────────────────────────────────────

static bool sensComboFired = false;

void checkSensitivityCombo(ControllerSlot &s) {
  // Home + Vol Up = increase, Home + Vol Down = decrease
  if (s.current.homeBtn && s.current.volUpBtn && !sensComboFired) {
    sensComboFired = true;
    float factor = 1.0f + SENSITIVITY_STEP;
    airMouseSensitivity *= factor;
    trackpadSensitivity *= factor;
    scrollSensitivity *= factor;
    Serial.printf("[SENS] ↑ air=%.0f tpad=%.1f scroll=%.1f\n",
                  airMouseSensitivity, trackpadSensitivity, scrollSensitivity);
    savePreferences();
    ledIndicateSensitivity(true);
  } else if (s.current.homeBtn && s.current.volDownBtn && !s.current.appBtn &&
             !sensComboFired) {
    // Only Vol Down (without App) = sensitivity decrease
    // App+VolDown is the controller switch combo
    sensComboFired = true;
    float factor = 1.0f - SENSITIVITY_STEP;
    airMouseSensitivity *= factor;
    trackpadSensitivity *= factor;
    scrollSensitivity *= factor;
    Serial.printf("[SENS] ↓ air=%.0f tpad=%.1f scroll=%.1f\n",
                  airMouseSensitivity, trackpadSensitivity, scrollSensitivity);
    savePreferences();
    ledIndicateSensitivity(false);
  }
  if (!s.current.homeBtn) {
    sensComboFired = false;
  }
}

// ─── Button Processing ──────────────────────────────────────────────────────

void processButtons(ControllerSlot &s) {
  bool comboActive = s.current.appBtn && s.current.volDownBtn;
  bool sensActive =
      s.current.homeBtn &&
      (s.current.volUpBtn || (s.current.volDownBtn && !s.current.appBtn));

  // ── Home Button ──
  if (s.current.homeBtn && !s.prevHomeBtn) {
    s.homePressSince = millis();
    s.homeLongFired = false;
  }

  if (s.current.homeBtn && !s.homeLongFired && !sensActive) {
    if (millis() - s.homePressSince >= HOME_LONG_PRESS_MS) {
      s.homeLongFired = true;
      s.hasRefOrientation = false;
      s.smoothDx = 0;
      s.smoothDy = 0;
      Serial.println("[MODE] Orientation recentered!");
      ledFlash(4, 40, 40);
      ledSetSolid();
    }
  }

  if (!s.current.homeBtn && s.prevHomeBtn && !s.homeLongFired && !sensActive) {
    int m = (int)currentMode + 1;
    if (m > MODE_MEDIA)
      m = MODE_AIR_MOUSE;
    currentMode = (DeviceMode)m;

    for (int i = 0; i < MAX_CONTROLLERS; i++) {
      slots[i].hasRefOrientation = false;
      slots[i].wasTouching = false;
      slots[i].swipeActive = false;
      slots[i].smoothDx = 0;
      slots[i].smoothDy = 0;
    }
    Serial.printf("[MODE] %s\n", modeNames[currentMode]);
    ledIndicateMode(currentMode);
  }

  // ── Sensitivity combo ──
  checkSensitivityCombo(s);

  // ── Mode-specific buttons (skip combos) ──
  if (currentMode == MODE_MEDIA) {
    if (s.current.clickBtn && !s.prevClickBtn) {
      ConsumerControl.press(CONSUMER_CONTROL_PLAY_PAUSE);
      ConsumerControl.release();
    }
    if (!comboActive && !sensActive && s.current.appBtn && !s.prevAppBtn) {
      ConsumerControl.press(CONSUMER_CONTROL_MUTE);
      ConsumerControl.release();
    }
    if (!sensActive && s.current.volUpBtn && !s.prevVolUpBtn) {
      ConsumerControl.press(CONSUMER_CONTROL_VOLUME_INCREMENT);
      ConsumerControl.release();
    }
    if (!comboActive && !sensActive && s.current.volDownBtn &&
        !s.prevVolDownBtn) {
      ConsumerControl.press(CONSUMER_CONTROL_VOLUME_DECREMENT);
      ConsumerControl.release();
    }
  } else {
    if (s.current.clickBtn && !s.prevClickBtn)
      Mouse.press(MOUSE_LEFT);
    else if (!s.current.clickBtn && s.prevClickBtn)
      Mouse.release(MOUSE_LEFT);

    if (!comboActive && !sensActive) {
      // App button = right click
      if (s.current.appBtn && !s.prevAppBtn)
        Mouse.press(MOUSE_RIGHT);
      else if (!s.current.appBtn && s.prevAppBtn)
        Mouse.release(MOUSE_RIGHT);

      // Volume buttons = scroll (continuous while held)
      if (s.current.volUpBtn)
        Mouse.move(0, 0, 1); // scroll up
      if (s.current.volDownBtn)
        Mouse.move(0, 0, -1); // scroll down
    }
  }

  s.prevClickBtn = s.current.clickBtn;
  s.prevHomeBtn = s.current.homeBtn;
  s.prevAppBtn = s.current.appBtn;
  s.prevVolDownBtn = s.current.volDownBtn;
  s.prevVolUpBtn = s.current.volUpBtn;
}

// ─── Serial Command Parser (Web Configurator) ──────────────────────────────
// Newline-delimited JSON protocol. Responses prefixed with '>' to distinguish
// from debug output. Browser dashboard filters lines starting with '>'.

static char serialBuf[256];
static int serialBufPos = 0;

// Helper: extract a float value from a JSON-ish string like ..."key":123.4...
static bool jsonFloat(const String &json, const char *key, float &out) {
  String needle = String("\"") + key + "\":";
  int idx = json.indexOf(needle);
  if (idx < 0)
    return false;
  idx += needle.length();
  out = json.substring(idx).toFloat();
  return true;
}

// Helper: extract an int value
static bool jsonInt(const String &json, const char *key, int &out) {
  String needle = String("\"") + key + "\":";
  int idx = json.indexOf(needle);
  if (idx < 0)
    return false;
  idx += needle.length();
  out = json.substring(idx).toInt();
  return true;
}

void sendConfig() {
  Serial.printf(">{\"type\":\"config\",\"air\":%.1f,\"tpad\":%.1f,\"scroll\":%."
                "1f,\"version\":\"1.1\"}\n",
                airMouseSensitivity, trackpadSensitivity, scrollSensitivity);
}

void sendStatus() {
  Serial.printf(
      ">{\"type\":\"status\",\"mode\":%d,\"modeName\":\"%s\",\"slot\":%d,"
      "\"c0\":%s,\"c1\":%s,\"sleep\":%s}\n",
      (int)currentMode, modeNames[currentMode], activeSlot,
      slots[0].connected ? "true" : "false",
      slots[1].connected ? "true" : "false", sleepMode ? "true" : "false");
}

void processSerialLine(const char *line) {
  String s(line);
  s.trim();
  if (s.length() == 0 || s.charAt(0) != '{')
    return;

  if (s.indexOf("\"get_config\"") >= 0) {
    sendConfig();
  } else if (s.indexOf("\"set_config\"") >= 0) {
    float val;
    if (jsonFloat(s, "air", val))
      airMouseSensitivity = constrain(val, 100.0f, 10000.0f);
    if (jsonFloat(s, "tpad", val))
      trackpadSensitivity = constrain(val, 0.5f, 50.0f);
    if (jsonFloat(s, "scroll", val))
      scrollSensitivity = constrain(val, 0.5f, 50.0f);
    savePreferences();
    sendConfig();
  } else if (s.indexOf("\"get_status\"") >= 0) {
    sendStatus();
  } else if (s.indexOf("\"set_mode\"") >= 0) {
    int m;
    if (jsonInt(s, "mode", m) && m >= 0 && m <= 2) {
      currentMode = (DeviceMode)m;
      ledIndicateMode(currentMode);
      Serial.printf("[MODE] Switched to %s (via serial)\n",
                    modeNames[currentMode]);
    }
    sendStatus();
  }
}

void processSerialCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBufPos > 0) {
        serialBuf[serialBufPos] = '\0';
        processSerialLine(serialBuf);
        serialBufPos = 0;
      }
    } else if (serialBufPos < (int)sizeof(serialBuf) - 1) {
      serialBuf[serialBufPos++] = c;
    }
  }
}

// ─── Setup ──────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("==========================================");
  Serial.println("  Daydream Air Mouse v1.1                 ");
  Serial.println("==========================================");
  Serial.println();

  // LED
  pinMode(LED_PIN, OUTPUT);
  ledSetBreathing();

  // Boot button
  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BOOT_BTN_PIN), bootBtnISR, FALLING);

  // Load saved preferences
  loadPreferences();

  // USB HID
  Mouse.begin();
  ConsumerControl.begin();
  USB.begin();
  Serial.println("[USB] HID ready");

  // BLE
  NimBLEDevice::init("DaydreamAirMouse");
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  Serial.println("[BLE] Ready (bonding, 2 slots)");

  lastControllerActivity = millis();
  startScan();

  Serial.println();
  Serial.printf("Mode: %s | Sensitivity: air=%.0f tpad=%.1f\n",
                modeNames[currentMode], airMouseSensitivity,
                trackpadSensitivity);
  Serial.println();
  Serial.println("Controls:");
  Serial.println("  Home (short)     → Cycle mode");
  Serial.println("  Home (hold 1s)   → Recenter");
  Serial.println("  App + Vol Down   → Switch controller");
  Serial.println("  Home + Vol Up    → Sensitivity ↑");
  Serial.println("  Home + Vol Down  → Sensitivity ↓");
}

// ─── Main Loop ──────────────────────────────────────────────────────────────

static unsigned long lastReconnectAttempt = 0;
static unsigned long lastStatusPrint = 0;

void loop() {
  // ── Serial command processing (Web Configurator) ──
  processSerialCommands();

  // ── LED breathing animation ──
  ledUpdateBreathing();

  // ── Auto-sleep: stop scanning if no controller found for 2 min ──
  bool anyConnected = false;
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    if (slots[i].connected) {
      anyConnected = true;
      break;
    }
  }

  if (!anyConnected && !sleepMode &&
      millis() - lastControllerActivity > AUTO_SLEEP_MS) {
    sleepMode = true;
    if (scanning) {
      NimBLEDevice::getScan()->stop();
      scanning = false;
    }
    ledSetOff();
    Serial.println(
        "[SLEEP] No controllers found — sleeping. Press Boot to wake.");
  }

  // ── Boot button: wake or switch ──
  if (bootBtnPressed) {
    bootBtnPressed = false;
    unsigned long now = millis();
    if (now - lastBootBtnTime > 300) {
      lastBootBtnTime = now;

      if (sleepMode) {
        sleepMode = false;
        lastControllerActivity = millis();
        Serial.println("[WAKE] Resuming scan...");
        ledSetBreathing();
        startScan();
      } else {
        int newSlot = (activeSlot + 1) % MAX_CONTROLLERS;
        if (slots[newSlot].connected) {
          activeSlot = newSlot;
          Serial.printf("[SWITCH] Slot %d\n", activeSlot);
          ledIndicateSlot(activeSlot);
        } else {
          ledFlash(5, 30, 30);
          if (anyConnected)
            ledSetSolid();
          else
            ledSetBreathing();
        }
      }
    }
  }

  // ── Handle pending connections ──
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    if (slots[i].doConnect) {
      slots[i].doConnect = false;
      if (connectToController(i)) {
        Serial.printf("[SLOT %d] Ready!\n", i);
        if (!slots[activeSlot].connected ||
            !slots[activeSlot].notificationsWorking) {
          activeSlot = i;
          Serial.printf("[SWITCH] Auto → Slot %d\n", activeSlot);
        }
        ledIndicateSlot(activeSlot);
      } else {
        slots[i].device = nullptr;
        slots[i].hasAddress = false;
        for (int j = 0; j < knownAddressCount; j++) {
          if (knownAddresses[j] == slots[i].address) {
            knownAddresses[j] = knownAddresses[knownAddressCount - 1];
            knownAddressCount--;
            break;
          }
        }
      }
    }
  }

  // ── Auto-reconnect / discovery ──
  if (!scanning && !sleepMode) {
    bool hasLostController = false;
    bool hasEmptySlot = false;

    for (int i = 0; i < MAX_CONTROLLERS; i++) {
      if (slots[i].connected)
        continue;
      if (slots[i].hasAddress && !slots[i].doConnect)
        hasLostController = true;
      else if (!slots[i].doConnect)
        hasEmptySlot = true;
    }

    unsigned long interval =
        hasLostController ? RECONNECT_DELAY_MS : DISCOVERY_INTERVAL_MS;
    bool needsScan = hasLostController || hasEmptySlot;

    if (needsScan) {
      unsigned long now = millis();
      if (now - lastReconnectAttempt > interval) {
        lastReconnectAttempt = now;
        for (int i = 0; i < MAX_CONTROLLERS; i++) {
          if (!slots[i].connected && !slots[i].doConnect)
            slots[i].device = nullptr;
        }
        startScan();
      }
    }
  }

  // ── Switch combo on ALL controllers ──
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    if (slots[i].connected && slots[i].notificationsWorking)
      checkSwitchCombo(slots[i]);
  }

  // ── Active controller data ──
  ControllerSlot &active = slots[activeSlot];
  if (active.connected && active.stateUpdated) {
    processMovement(active);
    processButtons(active);
  }

  // ── Inactive slots: update edge state ──
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    if (i != activeSlot && slots[i].stateUpdated) {
      slots[i].prevClickBtn = slots[i].current.clickBtn;
      slots[i].prevHomeBtn = slots[i].current.homeBtn;
      slots[i].prevAppBtn = slots[i].current.appBtn;
      slots[i].prevVolDownBtn = slots[i].current.volDownBtn;
      slots[i].prevVolUpBtn = slots[i].current.volUpBtn;
      slots[i].stateUpdated = false;
    }
  }

  // ── Status (every 15s) ──
  if (millis() - lastStatusPrint > 15000) {
    lastStatusPrint = millis();
    Serial.printf("[STATUS] Slot %d | %s | sens=%.0f%s\n", activeSlot,
                  modeNames[currentMode], airMouseSensitivity,
                  sleepMode ? " | SLEEPING" : "");
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
      if (slots[i].connected)
        Serial.printf("  Slot %d: %lu pkts\n", i, slots[i].notifyCount);
    }
  }

  delay(1);
}
