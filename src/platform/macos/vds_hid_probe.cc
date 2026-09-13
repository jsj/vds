// SPDX-License-Identifier: MIT
// Copyright (C) 2026 James Jones

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDDeviceKeys.h>
#include <IOKit/hidsystem/IOHIDUserDevice.h>
#include <mach/mach_time.h>

#include <array>
#include <csignal>
#include <cstdint>
#include <iostream>

#include "vds/ds5_usb.h"

namespace {

volatile std::sig_atomic_t running = 1;

void stop(int) { running = 0; }

void set_number(CFMutableDictionaryRef properties, CFStringRef key,
                std::int32_t value) {
  const auto number =
      CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
  CFDictionarySetValue(properties, key, number);
  CFRelease(number);
}

} // namespace

int main() {
  const auto properties = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  const auto descriptor =
      CFDataCreate(kCFAllocatorDefault, vds_ds5_usb_hid_report_descriptor,
                   sizeof(vds_ds5_usb_hid_report_descriptor));

  CFDictionarySetValue(properties, CFSTR(kIOHIDReportDescriptorKey),
                       descriptor);
  CFDictionarySetValue(properties, CFSTR(kIOHIDTransportKey), CFSTR("USB"));
  CFDictionarySetValue(properties, CFSTR(kIOHIDManufacturerKey),
                       CFSTR(VDS_USB_MANUFACTURER_STRING));
  CFDictionarySetValue(properties, CFSTR(kIOHIDProductKey),
                       CFSTR(VDS_DS5_USB_PRODUCT_STRING));
  set_number(properties, CFSTR(kIOHIDVendorIDKey), 0x054c);
  set_number(properties, CFSTR(kIOHIDProductIDKey), 0x0ce6);
  set_number(properties, CFSTR(kIOHIDVersionNumberKey), VDS_USB_DEVICE_BCD);

  const auto device =
      IOHIDUserDeviceCreateWithProperties(kCFAllocatorDefault, properties, 0);
  CFRelease(descriptor);
  CFRelease(properties);

  if (!device) {
    std::cerr << "failed to create virtual DualSense; the process needs the "
                 "com.apple.developer.hid.virtual.device entitlement\n";
    return 1;
  }

  std::signal(SIGINT, stop);
  std::signal(SIGTERM, stop);
  std::cout << "virtual USB DualSense created; press Ctrl-C to stop\n";

  std::array<std::uint8_t, VDS_USB_INPUT_REPORT_SIZE> report{};
  report[0] = 0x01;
  report[1] = report[2] = report[3] = report[4] = 0x80;
  while (running) {
    const auto result = IOHIDUserDeviceHandleReportWithTimeStamp(
        device, mach_absolute_time(), report.data(), report.size());
    if (result != kIOReturnSuccess) {
      std::cerr << "failed to submit input report: 0x" << std::hex << result
                << '\n';
      CFRelease(device);
      return 1;
    }
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.004, false);
  }

  CFRelease(device);
  return 0;
}
