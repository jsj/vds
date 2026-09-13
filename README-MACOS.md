# macOS

The macOS backend is experimental. Its first milestone is a virtual USB-style
DualSense HID device built from vDS's existing Sony report descriptor.

```sh
brew install cmake pkgconf opus
cmake -S . -B build-macos
cmake --build build-macos --target vds-macos-hid-probe
./build-macos/vds-macos-hid-probe
```

Apple requires the restricted `com.apple.developer.hid.virtual.device`
entitlement to create the virtual HID device. The build signs the probe ad hoc
with that entitlement so developers can test whether their macOS configuration
permits it.

On macOS 26.5 with Xcode 27, the ad-hoc signed probe registers as
`054C:0CE6`, matches Apple's PS5 game-controller personality, and is enumerated
by CrossOver 27 as `HID\\VID_054C&PID_0CE6`.
