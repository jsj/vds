// SPDX-License-Identifier: MIT
// Copyright (C) 2026 James Jones

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hid/IOHIDDeviceKeys.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hidsystem/IOHIDUserDevice.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>

#include <array>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <vector>

#include "vds/ds5_usb.h"
#include "vds_protocol.hh"

namespace {
volatile std::sig_atomic_t running = 1;

struct Bridge {
  IOHIDDeviceRef physical = nullptr;
  IOHIDUserDeviceRef virtual_device = nullptr;
  std::array<std::uint8_t, VDS_BT_STATE_REPORT_SIZE> input_buffer{};
  vds::DsOutputState output_state;
  std::uint64_t input_reports = 0;
  std::uint64_t output_reports = 0;
  std::uint64_t feature_gets = 0;
  std::uint64_t feature_sets = 0;
  bool virtual_device_cancelled = false;
};

void stop(int) { running = 0; }

void set_number(CFMutableDictionaryRef properties, CFStringRef key,
                std::int32_t value) {
  const auto number =
      CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
  CFDictionarySetValue(properties, key, number);
  CFRelease(number);
}

bool property_equals(IOHIDDeviceRef device, CFStringRef key,
                     CFStringRef expected) {
  const auto value = IOHIDDeviceGetProperty(device, key);
  return value && CFGetTypeID(value) == CFStringGetTypeID() &&
         CFStringCompare(static_cast<CFStringRef>(value), expected, 0) ==
             kCFCompareEqualTo;
}

IOHIDDeviceRef find_bluetooth_dualsense(IOHIDManagerRef manager) {
  const auto devices = IOHIDManagerCopyDevices(manager);
  if (!devices)
    return nullptr;
  const CFIndex count = CFSetGetCount(devices);
  std::vector<const void *> values(static_cast<std::size_t>(count));
  CFSetGetValues(devices, values.data());
  IOHIDDeviceRef found = nullptr;
  for (const void *value : values) {
    const auto device = (IOHIDDeviceRef)value;
    if (property_equals(device, CFSTR(kIOHIDTransportKey),
                        CFSTR("Bluetooth"))) {
      found = device;
      CFRetain(found);
      break;
    }
  }
  CFRelease(devices);
  return found;
}

void physical_input(void *context, IOReturn result, void *, IOHIDReportType,
                    std::uint32_t, std::uint8_t *report,
                    CFIndex report_length) {
  auto &bridge = *static_cast<Bridge *>(context);
  if (result != kIOReturnSuccess || report_length <= 0)
    return;
  std::vector<std::uint8_t> packet{0xa1};
  packet.insert(packet.end(), report, report + report_length);
  const auto usb_report = vds::bt_input_to_usb_input(packet);
  if (usb_report &&
      IOHIDUserDeviceHandleReportWithTimeStamp(
          bridge.virtual_device, mach_absolute_time(), usb_report->data(),
          usb_report->size()) == kIOReturnSuccess)
    ++bridge.input_reports;
}

IOHIDUserDeviceRef create_virtual_dualsense(Bridge &bridge) {
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
  set_number(properties, CFSTR(kIOHIDVendorIDKey), VDS_SONY_VENDOR_ID);
  set_number(properties, CFSTR(kIOHIDProductIDKey), VDS_DS5_PRODUCT_ID);
  set_number(properties, CFSTR(kIOHIDVersionNumberKey), VDS_USB_DEVICE_BCD);
  const auto device = IOHIDUserDeviceCreateWithProperties(
      kCFAllocatorDefault, properties, IOHIDUserDeviceOptionsCreateOnActivate);
  CFRelease(descriptor);
  CFRelease(properties);
  if (!device)
    return nullptr;

  IOHIDUserDeviceRegisterGetReportBlock(
      device, ^IOReturn(IOHIDReportType type, std::uint32_t report_id,
                        std::uint8_t *report, CFIndex *report_length) {
        if (type != kIOHIDReportTypeFeature || !report || !report_length ||
            *report_length <= 0)
          return kIOReturnUnsupported;

        std::array<std::uint8_t, 256> physical_report{};
        CFIndex physical_length = physical_report.size();
        const auto result = IOHIDDeviceGetReport(
            bridge.physical, kIOHIDReportTypeFeature, report_id,
            physical_report.data(), &physical_length);
        if (result != kIOReturnSuccess) {
          std::cerr << "feature GET 0x" << std::hex << report_id << std::dec
                    << " failed: 0x" << std::hex << result << std::dec << '\n';
          return result;
        }

        std::vector<std::uint8_t> usb_report;
        usb_report.reserve(static_cast<std::size_t>(physical_length) + 1);
        if (physical_length == 0 || physical_report[0] != report_id)
          usb_report.push_back(static_cast<std::uint8_t>(report_id));
        usb_report.insert(usb_report.end(), physical_report.begin(),
                          physical_report.begin() + physical_length);
        if (usb_report.size() > static_cast<std::size_t>(*report_length))
          usb_report.resize(static_cast<std::size_t>(*report_length));
        std::copy(usb_report.begin(), usb_report.end(), report);
        *report_length = static_cast<CFIndex>(usb_report.size());
        ++bridge.feature_gets;
        std::cout << "feature GET 0x" << std::hex << report_id << std::dec
                  << " -> " << usb_report.size() << " bytes\n";
        return kIOReturnSuccess;
      });

  IOHIDUserDeviceRegisterSetReportBlock(
      device, ^IOReturn(IOHIDReportType type, std::uint32_t report_id,
                        const std::uint8_t *report, CFIndex report_length) {
        if (report_length <= 0)
          return kIOReturnUnsupported;
        std::vector<std::uint8_t> usb_report(report, report + report_length);
        if (usb_report.front() != report_id)
          usb_report.insert(usb_report.begin(),
                            static_cast<std::uint8_t>(report_id));
        if (type == kIOHIDReportTypeFeature) {
          const auto packet = vds::feature_set_packet(usb_report);
          if (packet.size() < 2)
            return kIOReturnBadArgument;
          const auto result = IOHIDDeviceSetReport(
              bridge.physical, kIOHIDReportTypeFeature, report_id,
              packet.data() + 1, packet.size() - 1);
          if (result == kIOReturnSuccess)
            ++bridge.feature_sets;
          std::cout << "feature SET 0x" << std::hex << report_id << std::dec
                    << " <- " << usb_report.size() << " bytes, result=0x"
                    << std::hex << result << std::dec << '\n';
          return result;
        }
        if (type != kIOHIDReportTypeOutput)
          return kIOReturnUnsupported;
        if (!bridge.output_state.apply_usb_output_report(usb_report))
          return kIOReturnBadArgument;
        const auto bt_report = bridge.output_state.build_bt_state_report();
        const auto result = IOHIDDeviceSetReport(
            bridge.physical, kIOHIDReportTypeOutput, VDS_BT_STATE_REPORT_ID,
            bt_report.data(), bt_report.size());
        if (result == kIOReturnSuccess)
          ++bridge.output_reports;
        return result;
      });
  IOHIDUserDeviceSetDispatchQueue(device, dispatch_get_main_queue());
  IOHIDUserDeviceSetCancelHandler(
      device, ^{ bridge.virtual_device_cancelled = true; });
  IOHIDUserDeviceActivate(device);
  return device;
}
} // namespace

int main() {
  Bridge bridge;
  const auto manager = IOHIDManagerCreate(kCFAllocatorDefault, 0);
  const auto matching = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  set_number(matching, CFSTR(kIOHIDVendorIDKey), VDS_SONY_VENDOR_ID);
  set_number(matching, CFSTR(kIOHIDProductIDKey), VDS_DS5_PRODUCT_ID);
  IOHIDManagerSetDeviceMatching(manager, matching);
  CFRelease(matching);
  IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);

  bridge.physical = find_bluetooth_dualsense(manager);
  if (!bridge.physical ||
      IOHIDDeviceOpen(bridge.physical, kIOHIDOptionsTypeNone) !=
          kIOReturnSuccess) {
    std::cerr << "unable to open Bluetooth DualSense 054C:0CE6\n";
    if (bridge.physical)
      CFRelease(bridge.physical);
    CFRelease(manager);
    return 1;
  }
  bridge.virtual_device = create_virtual_dualsense(bridge);
  if (!bridge.virtual_device) {
    std::cerr << "failed to create virtual DualSense\n";
    IOHIDDeviceClose(bridge.physical, kIOHIDOptionsTypeNone);
    CFRelease(bridge.physical);
    CFRelease(manager);
    return 1;
  }

  IOHIDDeviceRegisterInputReportCallback(
      bridge.physical, bridge.input_buffer.data(), bridge.input_buffer.size(),
      physical_input, &bridge);
  IOHIDDeviceScheduleWithRunLoop(bridge.physical, CFRunLoopGetCurrent(),
                                 kCFRunLoopDefaultMode);
  std::signal(SIGINT, stop);
  std::signal(SIGTERM, stop);
  std::cout << "bridging Bluetooth DualSense to virtual USB HID; press Ctrl-C "
               "to stop\n";
  while (running)
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);

  std::cout << "forwarded input=" << bridge.input_reports
            << " output=" << bridge.output_reports
            << " feature_get=" << bridge.feature_gets
            << " feature_set=" << bridge.feature_sets << '\n';
  IOHIDDeviceUnscheduleFromRunLoop(bridge.physical, CFRunLoopGetCurrent(),
                                   kCFRunLoopDefaultMode);
  IOHIDUserDeviceCancel(bridge.virtual_device);
  while (!bridge.virtual_device_cancelled)
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
  CFRelease(bridge.virtual_device);
  IOHIDDeviceClose(bridge.physical, kIOHIDOptionsTypeNone);
  CFRelease(bridge.physical);
  IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
  CFRelease(manager);
  return 0;
}
