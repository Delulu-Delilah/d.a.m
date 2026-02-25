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
 *   - Boot button (GPIO 0) switches which controller is "active"
 *   - LED flashes: 1 = slot 0 active, 2 = slot 1 active
 *   - Both controllers stay connected; only active one drives HID output
 *
 * Modes (cycled by Home/Daydream button on active controller):
 *   - AIR MOUSE:  Orientation + accel fusion → mouse movement
 *                 Trackpad → scroll wheel
 *   - TRACKPAD:   Touchpad deltas → mouse movement
 *   - MEDIA:      Trackpad gestures → media control
 *
 * Home (short press): Cycle modes
 * Home (hold 1s):     Recenter orientation (air mouse mode)
 *
 * Supports both old and new Daydream controller firmware via
 * BLE bonding + explicit CCCD descriptor writes.
 */

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <USB.h>
#include <USBHIDConsumerControl.h>
#include <USBHIDMouse.h>

// ─── Device Modes ───────────────────────────────────────────────────────────

enum DeviceMode {
  MODE_AIR_MOUSE, // Orientation + accel → pointer, trackpad → scroll
  MODE_TRACKPAD,  // Touchpad → pointer
  MODE_MEDIA      // Trackpad gestures → media keys
};

static const char *modeNames[] = {"AIR MOUSE", "TRACKPAD", "MEDIA"};
static DeviceMode currentMode = MODE_AIR_MOUSE;

// ─── Configuration ──────────────────────────────────────────────────────────

static const float AIR_MOUSE_SENSITIVITY = 1800.0f;
static const float TRACKPAD_SENSITIVITY = 6.0f;
static const float SCROLL_SENSITIVITY = 8.0f;
static const float SMOOTH_ALPHA = 0.45f;

static const float AIR_MOUSE_DEADZONE = 0.003f;
static const float SCROLL_DEADZONE = 0.008f;
static const float SWIPE_THRESHOLD = 0.25f;

static const int BLE_SCAN_DURATION_SEC = 10;    // Scan for 10s then stop
static const int RECONNECT_DELAY_MS = 3000;     // Retry lost controllers
static const int DISCOVERY_INTERVAL_MS = 15000; // Look for new controllers
static const unsigned long HOME_LONG_PRESS_MS = 1000;

static const int LED_PIN = LED_BUILTIN;
static const int BOOT_BTN_PIN = 0; // Boot button on XIAO ESP32-S3

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
  // BLE connection
  NimBLEClient *client = nullptr;
  NimBLEAdvertisedDevice *device = nullptr;
  bool connected = false;
  bool doConnect = false;
  bool notificationsWorking = false;
  unsigned long connectTime = 0;
  unsigned long notifyCount = 0;
  unsigned long lastNotifyTime = 0;

  // Controller state
  DaydreamState current = {};
  DaydreamState previous = {};
  bool stateUpdated = false;

  // Per-controller tracking
  bool wasTouching = false;
  float prevTouchX = 0, prevTouchY = 0;
  float lastTouchX = 0;
  float swipeStartX = 0;
  bool swipeActive = false;
  bool hasRefOrientation = false;
  float refXOri = 0, refYOri = 0;
  float smoothDx = 0, smoothDy = 0;

  // Button edge detection
  bool prevClickBtn = false;
  bool prevHomeBtn = false;
  bool prevAppBtn = false;
  bool prevVolDownBtn = false;
  bool prevVolUpBtn = false;

  // Home button long-press
  unsigned long homePressSince = 0;
  bool homeLongFired = false;

  // Device address (for dedup)
  NimBLEAddress address;
  bool hasAddress = false;
};

// ─── Globals ────────────────────────────────────────────────────────────────

USBHIDMouse Mouse;
USBHIDConsumerControl ConsumerControl;

static ControllerSlot slots[MAX_CONTROLLERS];
static int activeSlot = 0;
static bool scanning = false;
static NimBLEAddress knownAddresses[MAX_CONTROLLERS];
static int knownAddressCount = 0;

// Boot button state
static volatile bool bootBtnPressed = false;
static unsigned long lastBootBtnTime = 0;

// ─── LED Helpers ────────────────────────────────────────────────────────────

void ledFlash(int count, int onMs = 100, int offMs = 100) {
  for (int i = 0; i < count; i++) {
    digitalWrite(LED_PIN, LOW);
    delay(onMs);
    digitalWrite(LED_PIN, HIGH);
    delay(offMs);
  }
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
  // 1 flash = slot 0, 2 flashes = slot 1
  ledFlash(slot + 1, 150, 150);
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

// ─── Find Slot by Client Pointer ────────────────────────────────────────────

int findSlotByClient(NimBLEClient *pClient) {
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    if (slots[i].client == pClient)
      return i;
  }
  return -1;
}

// ─── BLE Notification Callback ──────────────────────────────────────────────
// We use two separate callbacks so each knows which slot it belongs to.

void notifyCallbackSlot0(NimBLERemoteCharacteristic *pChar, uint8_t *data,
                         size_t length, bool isNotify) {
  ControllerSlot &s = slots[0];
  if (length >= 20) {
    s.previous = s.current;
    parsePacket(data, length, s.current);
    s.stateUpdated = true;
    s.lastNotifyTime = millis();
    s.notifyCount++;
    if (!s.notificationsWorking) {
      s.notificationsWorking = true;
      Serial.printf("[SLOT 0] ✓ First packet (%lu ms after connect)\n",
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
    if (!s.notificationsWorking) {
      s.notificationsWorking = true;
      Serial.printf("[SLOT 1] ✓ First packet (%lu ms after connect)\n",
                    millis() - s.connectTime);
    }
  }
}

// Array of callbacks indexed by slot
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
    Serial.printf("[SLOT %d] Connected!\n", slotIndex);
    ledFlash(3, 80, 80);
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
    Serial.printf("[SLOT %d] Disconnected (%lu packets)\n", slotIndex,
                  s.notifyCount);
    ledFlash(5, 50, 50);
  }

  uint32_t onPassKeyRequest() override { return 0; }

  bool onConfirmPIN(uint32_t pass_key) override { return true; }

  void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
    if (desc->sec_state.encrypted) {
      Serial.printf("[SLOT %d] ✓ Encrypted (bonded=%s)\n", slotIndex,
                    desc->sec_state.bonded ? "yes" : "no");
    } else {
      Serial.printf("[SLOT %d] ⚠ Not encrypted\n", slotIndex);
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
  // Also check for disconnected slots that had a previous device
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

      // Skip if we already know this controller
      if (isAddressKnown(addr)) {
        return;
      }

      int slot = findFreeSlot();
      if (slot < 0) {
        Serial.println("[BLE] No free slots, ignoring controller");
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

      // Track known addresses
      if (knownAddressCount < MAX_CONTROLLERS) {
        knownAddresses[knownAddressCount++] = addr;
      }

      // Check if we still need more controllers
      int connectedOrPending = 0;
      for (int i = 0; i < MAX_CONTROLLERS; i++) {
        if (slots[i].connected || slots[i].doConnect)
          connectedOrPending++;
      }

      if (connectedOrPending >= MAX_CONTROLLERS) {
        NimBLEDevice::getScan()->stop();
        scanning = false;
      }
      // Otherwise keep scanning for more controllers
    }
  }
};

static ScanCallbacks scanCB;

// ─── BLE Connect (per slot) ─────────────────────────────────────────────────

bool connectToController(int slotIdx) {
  ControllerSlot &s = slots[slotIdx];
  if (!s.device)
    return false;

  Serial.printf("[SLOT %d] Connecting...\n", slotIdx);

  if (!s.client) {
    s.client = NimBLEDevice::createClient();
    if (!clientCallbacks[slotIdx]) {
      clientCallbacks[slotIdx] = new ClientCallbacks(slotIdx);
    }
    s.client->setClientCallbacks(clientCallbacks[slotIdx]);
    s.client->setConnectionParams(12, 12, 0, 150);
    s.client->setConnectTimeout(10);
  }

  if (!s.client->connect(s.device)) {
    Serial.printf("[SLOT %d] Connection failed!\n", slotIdx);
    return false;
  }

  // Get service
  NimBLERemoteService *pService = s.client->getService(SERVICE_UUID);
  if (!pService) {
    Serial.printf("[SLOT %d] Service 0xFE55 not found!\n", slotIdx);
    s.client->disconnect();
    return false;
  }

  // Get characteristic
  NimBLERemoteCharacteristic *pChar = pService->getCharacteristic(CHAR_UUID);
  if (!pChar) {
    Serial.printf("[SLOT %d] Characteristic not found!\n", slotIdx);
    s.client->disconnect();
    return false;
  }

  // Request security/bonding (needed by updated firmware)
  Serial.printf("[SLOT %d] Requesting bonding...\n", slotIdx);
  s.client->secureConnection();
  delay(500);

  // Subscribe to notifications
  if (pChar->canNotify()) {
    if (!pChar->subscribe(true, slotCallbacks[slotIdx])) {
      Serial.printf("[SLOT %d] Standard subscribe failed, trying CCCD...\n",
                    slotIdx);
      NimBLERemoteDescriptor *cccd = pChar->getDescriptor(CCCD_UUID);
      if (cccd) {
        uint8_t enableNotify[] = {0x01, 0x00};
        cccd->writeValue(enableNotify, 2, true);
        pChar->subscribe(true, slotCallbacks[slotIdx]);
      }
    }
    Serial.printf("[SLOT %d] Subscribed\n", slotIdx);
  } else {
    Serial.printf("[SLOT %d] Cannot notify!\n", slotIdx);
    s.client->disconnect();
    return false;
  }

  // Wait for first notification
  unsigned long waitStart = millis();
  while (!s.notificationsWorking && millis() - waitStart < 3000) {
    delay(50);
  }

  if (s.notificationsWorking) {
    Serial.printf("[SLOT %d] ✓ Data stream active!\n", slotIdx);
  } else {
    Serial.printf("[SLOT %d] ⚠ No data yet, retrying CCCD...\n", slotIdx);
    pChar->unsubscribe();
    delay(200);
    NimBLERemoteDescriptor *cccd = pChar->getDescriptor(CCCD_UUID);
    if (cccd) {
      uint8_t enableNotify[] = {0x01, 0x00};
      cccd->writeValue(enableNotify, 2, true);
    }
    pChar->subscribe(true, slotCallbacks[slotIdx]);

    waitStart = millis();
    while (!s.notificationsWorking && millis() - waitStart < 3000) {
      delay(50);
    }
    if (s.notificationsWorking) {
      Serial.printf("[SLOT %d] ✓ Data stream active on retry!\n", slotIdx);
    } else {
      Serial.printf("[SLOT %d] ✗ Still no data\n", slotIdx);
    }
  }

  return true;
}

// ─── BLE Scan Start ─────────────────────────────────────────────────────────

void onScanComplete(NimBLEScanResults results) {
  scanning = false;
  Serial.printf("[BLE] Scan complete (%d results)\n", results.getCount());
}

void startScan() {
  if (scanning)
    return;

  // Count how many slots need a connection
  int needed = 0;
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    if (!slots[i].connected && !slots[i].doConnect)
      needed++;
  }
  if (needed == 0)
    return;

  Serial.printf("[BLE] Scanning (need %d controller%s)...\n", needed,
                needed > 1 ? "s" : "");
  scanning = true;

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

  float rawDx = -deltaYaw * AIR_MOUSE_SENSITIVITY;
  float rawDy = -deltaPitch * AIR_MOUSE_SENSITIVITY;

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
            (int)(-deltaScrollY * 255.0f * SCROLL_SENSITIVITY), -127, 127);
      }
    }
    s.prevTouchX = s.current.xTouch;
    s.prevTouchY = s.current.yTouch;
    s.wasTouching = true;
  } else {
    s.wasTouching = false;
  }

  if (dx != 0 || dy != 0 || scroll != 0) {
    Mouse.move(dx, dy, scroll);
  }
}

// ─── Trackpad Mode ──────────────────────────────────────────────────────────

void processTrackpad(ControllerSlot &s) {
  bool isTouching = (s.current.xTouch > 0.01f || s.current.yTouch > 0.01f);

  if (isTouching) {
    if (s.wasTouching) {
      float deltaX = (s.current.xTouch - s.prevTouchX) * 255.0f;
      float deltaY = (s.current.yTouch - s.prevTouchY) * 255.0f;

      float fx = deltaX * TRACKPAD_SENSITIVITY;
      float fy = deltaY * TRACKPAD_SENSITIVITY;

      int8_t dx = (int8_t)constrain((int)fx, -127, 127);
      int8_t dy = (int8_t)constrain((int)fy, -127, 127);

      if (dx != 0 || dy != 0) {
        Mouse.move(dx, dy, 0);
      }
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
      Serial.println("[MEDIA] >>> Next Track");
      ConsumerControl.press(CONSUMER_CONTROL_SCAN_NEXT);
      delay(50);
      ConsumerControl.release();
    } else if (swipeDelta < -SWIPE_THRESHOLD) {
      Serial.println("[MEDIA] <<< Previous Track");
      ConsumerControl.press(CONSUMER_CONTROL_SCAN_PREVIOUS);
      delay(50);
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
// App + Volume Down simultaneously on ANY controller → switch active slot

static bool switchComboFired = false;

void checkSwitchCombo(ControllerSlot &s) {
  bool comboHeld = s.current.appBtn && s.current.volDownBtn;
  bool comboJustPressed = comboHeld && (!s.prevAppBtn || !s.prevVolDownBtn);

  if (comboJustPressed && !switchComboFired) {
    switchComboFired = true;
    int newSlot = (activeSlot + 1) % MAX_CONTROLLERS;

    if (slots[newSlot].connected) {
      activeSlot = newSlot;
      Serial.printf("[SWITCH] Active: Slot %d (combo on controller)\n",
                    activeSlot);
      ledIndicateSlot(activeSlot);
    } else {
      Serial.printf("[SWITCH] Slot %d not connected\n", newSlot);
      ledFlash(5, 30, 30);
    }
  }

  // Reset combo when both buttons released
  if (!s.current.appBtn && !s.current.volDownBtn) {
    switchComboFired = false;
  }
}

// ─── Button Processing ──────────────────────────────────────────────────────

void processButtons(ControllerSlot &s) {
  // ── Switch combo check (App + Vol Down) ──
  // If combo is active, suppress individual App and VolDown actions
  bool comboActive = s.current.appBtn && s.current.volDownBtn;

  // ── Home Button ──
  if (s.current.homeBtn && !s.prevHomeBtn) {
    s.homePressSince = millis();
    s.homeLongFired = false;
  }

  if (s.current.homeBtn && !s.homeLongFired) {
    if (millis() - s.homePressSince >= HOME_LONG_PRESS_MS) {
      s.homeLongFired = true;
      s.hasRefOrientation = false;
      s.smoothDx = 0;
      s.smoothDy = 0;
      Serial.println("[MODE] Orientation recentered!");
      ledFlash(4, 40, 40);
    }
  }

  if (!s.current.homeBtn && s.prevHomeBtn && !s.homeLongFired) {
    int m = (int)currentMode + 1;
    if (m > MODE_MEDIA)
      m = MODE_AIR_MOUSE;
    currentMode = (DeviceMode)m;

    // Reset tracking for ALL slots on mode change
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
      slots[i].hasRefOrientation = false;
      slots[i].wasTouching = false;
      slots[i].swipeActive = false;
      slots[i].smoothDx = 0;
      slots[i].smoothDy = 0;
    }

    Serial.printf("[MODE] Switched to %s mode\n", modeNames[currentMode]);
    ledIndicateMode(currentMode);
  }

  // ── Mode-specific buttons (skip App/VolDown if combo active) ──
  if (currentMode == MODE_MEDIA) {
    if (s.current.clickBtn && !s.prevClickBtn) {
      ConsumerControl.press(CONSUMER_CONTROL_PLAY_PAUSE);
      delay(50);
      ConsumerControl.release();
    }
    if (!comboActive && s.current.appBtn && !s.prevAppBtn) {
      ConsumerControl.press(CONSUMER_CONTROL_MUTE);
      delay(50);
      ConsumerControl.release();
    }
    if (s.current.volUpBtn && !s.prevVolUpBtn) {
      ConsumerControl.press(CONSUMER_CONTROL_VOLUME_INCREMENT);
      delay(50);
      ConsumerControl.release();
    }
    if (!comboActive && s.current.volDownBtn && !s.prevVolDownBtn) {
      ConsumerControl.press(CONSUMER_CONTROL_VOLUME_DECREMENT);
      delay(50);
      ConsumerControl.release();
    }
  } else {
    if (s.current.clickBtn && !s.prevClickBtn)
      Mouse.press(MOUSE_LEFT);
    else if (!s.current.clickBtn && s.prevClickBtn)
      Mouse.release(MOUSE_LEFT);

    if (!comboActive) {
      if (s.current.appBtn && !s.prevAppBtn)
        Mouse.press(MOUSE_MIDDLE);
      else if (!s.current.appBtn && s.prevAppBtn)
        Mouse.release(MOUSE_MIDDLE);

      if (s.current.volDownBtn && !s.prevVolDownBtn)
        Mouse.press(MOUSE_RIGHT);
      else if (!s.current.volDownBtn && s.prevVolDownBtn)
        Mouse.release(MOUSE_RIGHT);
    }
  }

  // Update edge-detection state
  s.prevClickBtn = s.current.clickBtn;
  s.prevHomeBtn = s.current.homeBtn;
  s.prevAppBtn = s.current.appBtn;
  s.prevVolDownBtn = s.current.volDownBtn;
  s.prevVolUpBtn = s.current.volUpBtn;
}

// ─── Setup ──────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("==========================================");
  Serial.println("  Daydream Air Mouse — Dual Controller    ");
  Serial.println("==========================================");
  Serial.println();

  // LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  // Boot button for controller switching
  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BOOT_BTN_PIN), bootBtnISR, FALLING);

  // USB HID
  Mouse.begin();
  ConsumerControl.begin();
  USB.begin();
  Serial.println("[USB] HID initialized");

  // BLE with bonding support
  NimBLEDevice::init("DaydreamAirMouse");
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  Serial.println("[BLE] Initialized (bonding enabled, 2 slots)");

  startScan();

  Serial.println();
  Serial.printf("Active: Slot %d | Mode: %s\n", activeSlot,
                modeNames[currentMode]);
  Serial.println();
  Serial.println("Controls:");
  Serial.println("  Boot button:  Switch active controller");
  Serial.println("  Home (short): Cycle mode");
  Serial.println("  Home (hold):  Recenter orientation");
}

// ─── Main Loop ──────────────────────────────────────────────────────────────

static unsigned long lastReconnectAttempt = 0;
static unsigned long lastStatusPrint = 0;

void loop() {
  // ── Boot button: switch active controller ──
  if (bootBtnPressed) {
    bootBtnPressed = false;
    unsigned long now = millis();
    // Debounce: 300ms
    if (now - lastBootBtnTime > 300) {
      lastBootBtnTime = now;
      int newSlot = (activeSlot + 1) % MAX_CONTROLLERS;

      if (slots[newSlot].connected && slots[newSlot].notificationsWorking) {
        activeSlot = newSlot;
        Serial.printf("[SWITCH] Active: Slot %d\n", activeSlot);
        ledIndicateSlot(activeSlot);
      } else if (slots[newSlot].connected) {
        activeSlot = newSlot;
        Serial.printf("[SWITCH] Active: Slot %d (no data yet)\n", activeSlot);
        ledIndicateSlot(activeSlot);
      } else {
        Serial.printf("[SWITCH] Slot %d not connected, staying on Slot %d\n",
                      newSlot, activeSlot);
        ledFlash(5, 30, 30); // Rapid flash = error
      }
    }
  }

  // ── Handle pending connections ──
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    if (slots[i].doConnect) {
      slots[i].doConnect = false;
      if (connectToController(i)) {
        Serial.printf("[SLOT %d] Ready!\n", i);

        // If no active slot is connected, auto-switch to this one
        if (!slots[activeSlot].connected ||
            !slots[activeSlot].notificationsWorking) {
          activeSlot = i;
          Serial.printf("[SWITCH] Auto-selected Slot %d\n", activeSlot);
          ledIndicateSlot(activeSlot);
        }
      } else {
        Serial.printf("[SLOT %d] Connection failed, will retry\n", i);
        slots[i].device = nullptr;
        slots[i].hasAddress = false;
        // Remove from known addresses
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

  // ── Auto-reconnect / scan for more controllers ──
  if (!scanning) {
    // Check what we need: reconnect a lost controller vs discover a new one
    bool hasLostController = false;
    bool hasEmptySlot = false;
    int connectedCount = 0;

    for (int i = 0; i < MAX_CONTROLLERS; i++) {
      if (slots[i].connected) {
        connectedCount++;
      } else if (slots[i].hasAddress && !slots[i].doConnect) {
        hasLostController = true; // Previously connected, now lost
      } else if (!slots[i].doConnect) {
        hasEmptySlot = true; // Never had a controller in this slot
      }
    }

    // Use shorter interval for reconnecting lost controllers,
    // longer interval for discovering new ones
    unsigned long interval =
        hasLostController ? RECONNECT_DELAY_MS : DISCOVERY_INTERVAL_MS;
    bool needsScan = hasLostController || hasEmptySlot;

    if (needsScan) {
      unsigned long now = millis();
      if (now - lastReconnectAttempt > interval) {
        lastReconnectAttempt = now;

        // Clear stale device references for disconnected slots
        for (int i = 0; i < MAX_CONTROLLERS; i++) {
          if (!slots[i].connected && !slots[i].doConnect) {
            slots[i].device = nullptr;
          }
        }
        startScan();
      }
    }
  }

  // ── Check switch combo on ALL connected controllers ──
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    if (slots[i].connected && slots[i].notificationsWorking) {
      checkSwitchCombo(slots[i]);
    }
  }

  // ── Process active controller data ──
  ControllerSlot &active = slots[activeSlot];
  if (active.connected && active.stateUpdated) {
    processMovement(active);
    processButtons(active);
  }

  // ── Update edge-detection state for inactive slots ──
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    if (i != activeSlot && slots[i].stateUpdated) {
      // Update button prev-state so combo detection works next frame
      slots[i].prevClickBtn = slots[i].current.clickBtn;
      slots[i].prevHomeBtn = slots[i].current.homeBtn;
      slots[i].prevAppBtn = slots[i].current.appBtn;
      slots[i].prevVolDownBtn = slots[i].current.volDownBtn;
      slots[i].prevVolUpBtn = slots[i].current.volUpBtn;
      slots[i].stateUpdated = false;
    }
  }

  // ── Periodic status ──
  if (millis() - lastStatusPrint > 15000) {
    lastStatusPrint = millis();
    Serial.printf("[STATUS] Active: Slot %d | Mode: %s\n", activeSlot,
                  modeNames[currentMode]);
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
      if (slots[i].connected) {
        Serial.printf("  Slot %d: %lu pkts | %s\n", i, slots[i].notifyCount,
                      slots[i].notificationsWorking ? "OK" : "NO DATA");
      } else {
        Serial.printf("  Slot %d: disconnected\n", i);
      }
    }
  }

  delay(1);
}
