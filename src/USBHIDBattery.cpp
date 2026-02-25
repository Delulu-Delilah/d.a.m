#include "USBHIDBattery.h"
#include <USBHID.h>

// HID Report Descriptor for a Battery System
// Exposes a single 1-byte Input report containing the remaining capacity
// (percentage)
static const uint8_t battery_report_descriptor[] = {
    0x05, 0x85, // Usage Page (Battery System)
    0x09, 0x01, // Usage (Battery System)
    0xa1, 0x01, // Collection (Application)
    0x09, 0x66, //   Usage (Remaining Capacity)
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0x64, //   Logical Maximum (100)
    0x75, 0x08, //   Report Size (8 bits)
    0x95, 0x01, //   Report Count (1)
    0x81, 0x02, //   Input (Data, Variable, Absolute)
    0xc0        // End Collection
};

USBHIDBattery::USBHIDBattery() : batteryLevel(100) {}

void USBHIDBattery::begin() {
  hid.addDevice(this, sizeof(battery_report_descriptor));
}

uint16_t USBHIDBattery::_onGetDescriptor(uint8_t *buffer) {
  memcpy(buffer, battery_report_descriptor, sizeof(battery_report_descriptor));
  return sizeof(battery_report_descriptor);
}

void USBHIDBattery::setBatteryLevel(uint8_t level) {
  if (level > 100)
    level = 100;

  batteryLevel = level;
  uint8_t report[1] = {batteryLevel};

  if (hid.ready()) {
    hid.SendReport(0, report, sizeof(report));
  }
}
