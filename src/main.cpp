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

#include "USBHIDBattery.h"
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

static NimBLEUUID BATTERY_SERVICE_UUID((uint16_t)0x180f);
static NimBLEUUID BATTERY_CHAR_UUID((uint16_t)0x2a19);

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

  int batteryLevel = -1;               // -1 means unknown
  unsigned long scrollRepeatTimer = 0; // rate limit continuous scrolling

  // Track combo states per-slot to avoid interference
  bool switchComboFired = false;
  bool sensComboFired = false;
};

static portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
static unsigned long globalMediaReleaseTimer = 0;

// ─── Globals ────────────────────────────────────────────────────────────────

USBHIDMouse Mouse;
USBHIDConsumerControl ConsumerControl;
USBHIDBattery BatteryReport;
Preferences prefs;

static ControllerSlot slots[MAX_CONTROLLERS];
static int activeSlot = 0;
static bool scanning = false;

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

// Flash state tracking
static int ledFlashesRemaining = 0;
static int ledFlashOnMs = 100;
static int ledFlashOffMs = 100;
static bool ledFlashIsOn = false;
static LEDState ledStateAfterFlash = LED_STATE_OFF;

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

static unsigned long lastSensChangeTime = 0;
static bool pendingPrefsSave = false;

void savePreferences() {
  prefs.begin("ddmouse", false);
  prefs.putFloat("airSens", airMouseSensitivity);
  prefs.putFloat("tpadSens", trackpadSensitivity);
  prefs.putFloat("scrollSens", scrollSensitivity);
  prefs.end();
  Serial.printf("[PREF] Saved: air=%.0f tpad=%.1f scroll=%.1f\n",
                airMouseSensitivity, trackpadSensitivity, scrollSensitivity);
}

void deferPreferencesSave() {
  pendingPrefsSave = true;
  lastSensChangeTime = millis();
}

// ─── LED Helpers ────────────────────────────────────────────────────────────

void ledFlash(int count, int onMs = 100, int offMs = 100,
              int forceAfterState = -1) {
  if (forceAfterState >= 0) {
    ledStateAfterFlash = (LEDState)forceAfterState;
  } else if (ledState != LED_STATE_FLASH) {
    ledStateAfterFlash = ledState;
  }
  ledState = LED_STATE_FLASH;
  ledFlashesRemaining = count;
  ledFlashOnMs = onMs;
  ledFlashOffMs = offMs;
  ledFlashIsOn = true;
  ledLastUpdate = millis();
  LED_ON();
}

// Non-blocking LED update (call from loop)
void ledUpdate() {
  unsigned long now = millis();

  if (ledState == LED_STATE_FLASH) {
    if (ledFlashesRemaining > 0) {
      if (ledFlashIsOn && (now - ledLastUpdate >= ledFlashOnMs)) {
        ledFlashIsOn = false;
        ledLastUpdate = now;
        LED_OFF();
      } else if (!ledFlashIsOn && (now - ledLastUpdate >= ledFlashOffMs)) {
        ledFlashesRemaining--;
        if (ledFlashesRemaining > 0) {
          ledFlashIsOn = true;
          ledLastUpdate = now;
          LED_ON();
        } else {
          ledState = ledStateAfterFlash;
          if (ledState == LED_STATE_SOLID)
            LED_ON();
          else if (ledState == LED_STATE_OFF)
            LED_OFF();
        }
      }
    } else {
      ledState = ledStateAfterFlash;
      if (ledState == LED_STATE_SOLID)
        LED_ON();
      else if (ledState == LED_STATE_OFF)
        LED_OFF();
    }
  } else if (ledState == LED_STATE_BREATHING) {
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
}

void ledIndicateSlot(int slot) {
  ledFlash(slot + 1, 150, 150, LED_STATE_SOLID);
}

void ledIndicateSensitivity(bool increase) {
  // Quick double-flash for up, triple for down
  ledFlash(increase ? 2 : 3, 40, 40, LED_STATE_SOLID);
}

void ledIndicateBattery(int level) {
  // level 0-3: flash count indicates battery (0=empty, 3=full)
  int flashes = constrain(level, 1, 4);
  ledFlash(flashes, 200, 200);
}

// ─── Boot Button ISR ────────────────────────────────────────────────────────

void IRAM_ATTR bootBtnISR() { bootBtnPressed = true; }

// ─── Packet Parser ──────────────────────────────────────────────────────────

static int16_t signExtend13(int raw) {
  raw &= 0x1FFF; // Prevent garbage bits >12 from breaking sign extension
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

void updateBatteryReport(int slotIdx) {
  if (slotIdx == activeSlot && slots[slotIdx].batteryLevel >= 0) {
    BatteryReport.setBatteryLevel((uint8_t)slots[slotIdx].batteryLevel);
  }
}

static void notifyCallback(NimBLERemoteCharacteristic *pChar, uint8_t *pData,
                           size_t length, bool isNotify, int slotIdx) {
  ControllerSlot &s = slots[slotIdx];
  if (length >= 20) {
    portENTER_CRITICAL(&stateMux);
    s.previous = s.current;
    parsePacket(pData, length, s.current);
    s.stateUpdated = true;
    portEXIT_CRITICAL(&stateMux);

    s.lastNotifyTime = millis();
    s.notifyCount++;
    lastControllerActivity = millis();
    if (!s.notificationsWorking) {
      s.notificationsWorking = true;
      Serial.printf("[SLOT %d] ✓ First packet (%lu ms)\n", slotIdx,
                    millis() - s.connectTime);
    }
  }
}

void notifyCallbackSlot0(NimBLERemoteCharacteristic *pChar, uint8_t *data,
                         size_t length, bool isNotify) {
  notifyCallback(pChar, data, length, isNotify, 0);
}

void notifyCallbackSlot1(NimBLERemoteCharacteristic *pChar, uint8_t *data,
                         size_t length, bool isNotify) {
  notifyCallback(pChar, data, length, isNotify, 1);
}

typedef void (*NotifyCallback)(NimBLERemoteCharacteristic *, uint8_t *, size_t,
                               bool);
static NotifyCallback slotCallbacks[MAX_CONTROLLERS] = {notifyCallbackSlot0,
                                                        notifyCallbackSlot1};

static void batteryNotifyCallback(NimBLERemoteCharacteristic *pChar,
                                  uint8_t *pData, size_t length, bool isNotify,
                                  int slotIdx) {
  if (slotIdx < 0 || slotIdx >= MAX_CONTROLLERS)
    return;
  if (length > 0) {
    slots[slotIdx].batteryLevel = pData[0];
    updateBatteryReport(slotIdx);
  }
}

void batteryNotifyCallbackSlot0(NimBLERemoteCharacteristic *pChar,
                                uint8_t *data, size_t length, bool isNotify) {
  batteryNotifyCallback(pChar, data, length, isNotify, 0);
}

void batteryNotifyCallbackSlot1(NimBLERemoteCharacteristic *pChar,
                                uint8_t *data, size_t length, bool isNotify) {
  batteryNotifyCallback(pChar, data, length, isNotify, 1);
}

static NotifyCallback batteryCallbacks[MAX_CONTROLLERS] = {
    batteryNotifyCallbackSlot0, batteryNotifyCallbackSlot1};

// ─── BLE Client Callbacks ─────────────────────────────────────────────

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
    s.stateUpdated = false;
    s.hasRefOrientation = false;

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

bool isAddressKnown(NimBLEAddress addr) {
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    // Only return true if the slot is actually connected
    // This allows re-discovering a previously known but disconnected controller
    if (slots[i].hasAddress && slots[i].address == addr && slots[i].connected)
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

      slots[slot].doConnect = true;
      slots[slot].address = addr;
      slots[slot].hasAddress = true;

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
  if (!s.hasAddress)
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

  if (!s.client->connect(s.address, false)) { // connect to address directly
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
    Serial.printf("[SLOT %d] Data stream active!\n", slotIdx);
  } else {
    Serial.printf("[SLOT %d] No data\n", slotIdx);
  }

  // Discover Battery Service (0x180f) and subscribe to fix LED flashing
  NimBLERemoteService *pBatService = s.client->getService(BATTERY_SERVICE_UUID);
  if (pBatService) {
    NimBLERemoteCharacteristic *pBatChar =
        pBatService->getCharacteristic(BATTERY_CHAR_UUID);
    if (pBatChar && pBatChar->canNotify()) {
      pBatChar->subscribe(true, batteryCallbacks[slotIdx], false);
      Serial.printf("[SLOT %d] Subscribed to battery notifications\n", slotIdx);
    } else {
      Serial.printf("[SLOT %d] Battery char missing or cannot notify\n",
                    slotIdx);
    }
  } else {
    Serial.printf("[SLOT %d] Battery service not found\n", slotIdx);
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

void processAirMouse(ControllerSlot &s, const DaydreamState &c,
                     const DaydreamState &p) {
  if (!s.hasRefOrientation) {
    s.refXOri = c.xOri;
    s.refYOri = c.yOri;
    s.hasRefOrientation = true;
    return;
  }

  float deltaYaw = c.yOri - p.yOri;
  float deltaPitch = c.xOri - p.xOri;

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
  // Note: (0, 0) is treated as "not touching" intentionally, as the Daydream
  // protocol encodes the lack of touch as literal zeroes.
  bool isTouching = (c.xTouch > 0.01f || c.yTouch > 0.01f);

  if (isTouching) {
    if (s.wasTouching) {
      float deltaScrollY = (c.yTouch - s.prevTouchY);
      if (fabsf(deltaScrollY) > SCROLL_DEADZONE) {
        scroll = (int8_t)constrain(
            (int)(-deltaScrollY * 255.0f * scrollSensitivity), -127, 127);
      }
    }
    s.prevTouchX = c.xTouch;
    s.prevTouchY = c.yTouch;
    s.wasTouching = true;
  } else {
    s.wasTouching = false;
  }

  if (dx != 0 || dy != 0 || scroll != 0)
    Mouse.move(dx, dy, scroll);
}

// ─── Trackpad Mode ──────────────────────────────────────────────────────────

void processTrackpad(ControllerSlot &s, const DaydreamState &c) {
  bool isTouching = (c.xTouch > 0.01f || c.yTouch > 0.01f);

  if (isTouching) {
    if (s.wasTouching) {
      float deltaX = (c.xTouch - s.prevTouchX) * 255.0f;
      float deltaY = (c.yTouch - s.prevTouchY) * 255.0f;

      float fx = deltaX * trackpadSensitivity;
      float fy = deltaY * trackpadSensitivity;

      int8_t dx = (int8_t)constrain((int)fx, -127, 127);
      int8_t dy = (int8_t)constrain((int)fy, -127, 127);

      if (dx != 0 || dy != 0)
        Mouse.move(dx, dy, 0);
    }
    s.prevTouchX = c.xTouch;
    s.prevTouchY = c.yTouch;
    s.wasTouching = true;
  } else {
    s.wasTouching = false;
  }
}

// ─── Media Mode Gestures ────────────────────────────────────────────────────

void processMediaGestures(ControllerSlot &s, const DaydreamState &c) {
  bool isTouching = (c.xTouch > 0.01f || c.yTouch > 0.01f);

  if (isTouching) {
    s.lastTouchX = c.xTouch;
    if (!s.swipeActive) {
      s.swipeStartX = c.xTouch;
      s.swipeActive = true;
    }
  } else if (s.swipeActive) {
    float swipeDelta = s.lastTouchX - s.swipeStartX;

    if (swipeDelta > SWIPE_THRESHOLD) {
      ConsumerControl.press(CONSUMER_CONTROL_SCAN_NEXT);
      globalMediaReleaseTimer = millis() + 50;
    } else if (swipeDelta < -SWIPE_THRESHOLD) {
      ConsumerControl.press(CONSUMER_CONTROL_SCAN_PREVIOUS);
      globalMediaReleaseTimer = millis() + 50;
    }
    s.swipeActive = false;
  }
}

// ─── Movement Dispatcher ────────────────────────────────────────────────────

void processMovement(ControllerSlot &s, const DaydreamState &c,
                     const DaydreamState &p) {
  switch (currentMode) {
  case MODE_AIR_MOUSE:
    processAirMouse(s, c, p);
    break;
  case MODE_TRACKPAD:
    processTrackpad(s, c);
    break;
  case MODE_MEDIA:
    processMediaGestures(s, c);
    break;
  }
}

// ─── Controller Switch Combo ────────────────────────────────────────────────

void checkSwitchCombo(ControllerSlot &s, const DaydreamState &c) {
  bool comboHeld = c.appBtn && c.volDownBtn;
  bool comboJustPressed = comboHeld && (!s.prevAppBtn || !s.prevVolDownBtn);

  if (comboJustPressed && !s.switchComboFired) {
    s.switchComboFired = true;
    int newSlot = (activeSlot + 1) % MAX_CONTROLLERS;
    if (slots[newSlot].connected) {
      activeSlot = newSlot;
      Serial.printf("[SWITCH] Active: Slot %d\n", activeSlot);
      ledIndicateSlot(activeSlot);
    } else {
      ledFlash(5, 30, 30);
    }
  }
  if (!c.appBtn && !c.volDownBtn) {
    s.switchComboFired = false;
  }
}

// ─── Sensitivity Adjustment ─────────────────────────────────────────────────

void checkSensitivityCombo(ControllerSlot &s, const DaydreamState &c) {
  // Home + Vol Up = increase, Home + Vol Down = decrease
  if (c.homeBtn && c.volUpBtn && !s.sensComboFired) {
    s.sensComboFired = true;
    float factor = 1.0f + SENSITIVITY_STEP;
    airMouseSensitivity =
        constrain(airMouseSensitivity * factor, 100.0f, 10000.0f);
    trackpadSensitivity = constrain(trackpadSensitivity * factor, 0.5f, 50.0f);
    scrollSensitivity = constrain(scrollSensitivity * factor, 0.5f, 50.0f);
    Serial.printf("[SENS] ↑ air=%.0f tpad=%.1f scroll=%.1f\n",
                  airMouseSensitivity, trackpadSensitivity, scrollSensitivity);
    deferPreferencesSave();
    ledIndicateSensitivity(true);
  } else if (c.homeBtn && c.volDownBtn && !c.appBtn && !s.sensComboFired) {
    // Only Vol Down (without App) = sensitivity decrease
    // App+VolDown is the controller switch combo
    s.sensComboFired = true;
    float factor = 1.0f - SENSITIVITY_STEP;
    airMouseSensitivity =
        constrain(airMouseSensitivity * factor, 100.0f, 10000.0f);
    trackpadSensitivity = constrain(trackpadSensitivity * factor, 0.5f, 50.0f);
    scrollSensitivity = constrain(scrollSensitivity * factor, 0.5f, 50.0f);
    Serial.printf("[SENS] ↓ air=%.0f tpad=%.1f scroll=%.1f\n",
                  airMouseSensitivity, trackpadSensitivity, scrollSensitivity);
    deferPreferencesSave();
    ledIndicateSensitivity(false);
  }
  if (!c.homeBtn) {
    s.sensComboFired = false;
  }
}

// ─── Button Processing ──────────────────────────────────────────────────────

void processButtons(ControllerSlot &s, const DaydreamState &c) {
  bool comboActive = (c.appBtn && c.volDownBtn);
  bool sensActive = c.homeBtn && (c.volUpBtn || (c.volDownBtn && !c.appBtn));

  // ── Home Button ──
  if (c.homeBtn && !s.prevHomeBtn) {
    s.homePressSince = millis();
    s.homeLongFired = false;
  }

  if (c.homeBtn && !s.homeLongFired && !sensActive) {
    if (millis() - s.homePressSince >= HOME_LONG_PRESS_MS) {
      s.homeLongFired = true;
      s.hasRefOrientation = false;
      s.smoothDx = 0;
      s.smoothDy = 0;
      Serial.println("[MODE] Orientation recentered!");
      ledFlash(4, 40, 40, LED_STATE_SOLID);
    }
  }

  if (!c.homeBtn && s.prevHomeBtn && !s.homeLongFired && !sensActive) {
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

  checkSensitivityCombo(s, c);

  // ── Mode-specific buttons (skip combos) ──
  if (currentMode == MODE_MEDIA) {
    if (c.clickBtn && !s.prevClickBtn) {
      ConsumerControl.press(CONSUMER_CONTROL_PLAY_PAUSE);
      globalMediaReleaseTimer = millis() + 50;
    }
    if (!comboActive && !sensActive && c.appBtn && !s.prevAppBtn) {
      ConsumerControl.press(CONSUMER_CONTROL_MUTE);
      globalMediaReleaseTimer = millis() + 50;
    }
    if (!sensActive && c.volUpBtn && !s.prevVolUpBtn) {
      ConsumerControl.press(CONSUMER_CONTROL_VOLUME_INCREMENT);
      globalMediaReleaseTimer = millis() + 50;
    }
    if (!comboActive && !sensActive && c.volDownBtn && !s.prevVolDownBtn) {
      ConsumerControl.press(CONSUMER_CONTROL_VOLUME_DECREMENT);
      globalMediaReleaseTimer = millis() + 50;
    }
  } else {
    if (c.clickBtn && !s.prevClickBtn)
      Mouse.press(MOUSE_LEFT);
    else if (!c.clickBtn && s.prevClickBtn)
      Mouse.release(MOUSE_LEFT);

    if (!comboActive && !sensActive) {
      // App button = right click
      if (c.appBtn && !s.prevAppBtn)
        Mouse.press(MOUSE_RIGHT);
      else if (!c.appBtn && s.prevAppBtn)
        Mouse.release(MOUSE_RIGHT);

      // Volume buttons = scroll (continuous while held)
      if ((c.volUpBtn || c.volDownBtn) && millis() > s.scrollRepeatTimer) {
        if (c.volUpBtn)
          Mouse.move(0, 0, 1); // scroll up
        if (c.volDownBtn)
          Mouse.move(0, 0, -1);              // scroll down
        s.scrollRepeatTimer = millis() + 50; // 50ms rate limit
      }
    }
  }

  s.prevClickBtn = c.clickBtn;
  s.prevHomeBtn = c.homeBtn;
  s.prevAppBtn = c.appBtn;
  s.prevVolDownBtn = c.volDownBtn;
  s.prevVolUpBtn = c.volUpBtn;
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
      "\"c0\":%s,\"c1\":%s,\"sleep\":%s,\"c0_bat\":%d,\"c1_bat\":%d}\n",
      (int)currentMode, modeNames[currentMode], activeSlot,
      slots[0].connected ? "true" : "false",
      slots[1].connected ? "true" : "false", sleepMode ? "true" : "false",
      slots[0].batteryLevel, slots[1].batteryLevel);
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

  // Active slot changed: update battery report
  static int prevReportedActiveSlot = -1;
  if (prevReportedActiveSlot != activeSlot) {
    updateBatteryReport(activeSlot);
    prevReportedActiveSlot = activeSlot;
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

  // USB HID (Mouse/Consumer must begin() BEFORE USB.begin() on ESP32-S3)
  Mouse.begin();
  ConsumerControl.begin();
  BatteryReport.begin();
  USB.begin();

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
  // ── Save deferred preferences to flash (avoid NVS wear) ──
  if (pendingPrefsSave && millis() - lastSensChangeTime > 3000) {
    savePreferences();
    pendingPrefsSave = false;
  }

  // ── Serial command processing (Web Configurator) ──
  processSerialCommands();

  // ── LED update (handles breathing + flash timing without blocking) ──
  ledUpdate();

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
        lastControllerActivity = millis(); // Reset sleep timer on wake/switch
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
        slots[i].hasAddress = false;
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
          if (!slots[i].connected && !slots[i].doConnect) {
            slots[i].hasAddress = false; // Clear limbo state
          }
        }
        startScan();
      }
    }
  }

  // ── Switch combo on ALL controllers ──
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    if (slots[i].connected && slots[i].notificationsWorking) {
      DaydreamState c;
      portENTER_CRITICAL(&stateMux);
      c = slots[i].current;
      portEXIT_CRITICAL(&stateMux);

      checkSwitchCombo(slots[i], c);
    }
  }

  // ── Active controller data ──
  ControllerSlot &active = slots[activeSlot];
  bool doProcess = false;
  DaydreamState lockedCurrent, lockedPrevious;

  portENTER_CRITICAL(&stateMux);
  if (active.connected && active.stateUpdated) {
    lockedCurrent = active.current;
    lockedPrevious = active.previous;
    active.stateUpdated = false;
    doProcess = true;
  }
  portEXIT_CRITICAL(&stateMux);

  if (doProcess) {
    processMovement(active, lockedCurrent, lockedPrevious);
    processButtons(active, lockedCurrent);
  }

  // ── Inactive slots: update edge state ──
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    if (i != activeSlot) {
      DaydreamState c;
      bool updated = false;

      portENTER_CRITICAL(&stateMux);
      if (slots[i].stateUpdated) {
        c = slots[i].current;
        slots[i].stateUpdated = false;
        updated = true;
      }
      portEXIT_CRITICAL(&stateMux);

      if (updated) {
        slots[i].prevClickBtn = c.clickBtn;
        slots[i].prevHomeBtn = c.homeBtn;
        slots[i].prevAppBtn = c.appBtn;
        slots[i].prevVolDownBtn = c.volDownBtn;
        slots[i].prevVolUpBtn = c.volUpBtn;
      }
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

  // ── Non-blocking media key release ──
  if (globalMediaReleaseTimer > 0 && millis() > globalMediaReleaseTimer) {
    ConsumerControl.release();
    globalMediaReleaseTimer = 0;
  }
  delay(1);
}
