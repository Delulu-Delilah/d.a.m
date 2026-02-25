#ifndef _USBHIDBATTERY_H_
#define _USBHIDBATTERY_H_

#include <Arduino.h>
#include <USBHID.h>

class USBHIDBattery : public USBHIDDevice {
private:
  USBHID hid;
  uint8_t batteryLevel;

public:
  USBHIDBattery();
  void begin();
  uint16_t _onGetDescriptor(uint8_t *buffer);
  void setBatteryLevel(uint8_t level); // 0-100%
};

#endif
