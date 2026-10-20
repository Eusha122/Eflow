#pragma once
#include <windows.h>

#include <map>
#include <string>
#include <vector>

namespace eflow {

// Things plugged into the PC that the island announces: USB drives / memory cards (with Open and Eject), phones by cable,
// and any other USB device (Wi-Fi and Bluetooth adapters, keyboards, mice, cameras, ...). Drives arrive as WM_DEVICECHANGE
// broadcasts to every top-level window; phones and USB devices need registrations. The app forwards WM_DEVICECHANGE to
// OnDeviceChange. A USB device is identified a moment later (Windows needs time to set it up): it arrives as `usbMessage`
// with a heap-allocated DeviceEvent in lParam.
struct DeviceEvent {
    enum class Kind { DriveAdded, DriveRemoved, PhoneAdded, PhoneRemoved, UsbAdded };
    enum class Usb { Storage, WiFi, Network, Bluetooth, Keyboard, Mouse, Input, Camera, Printer, Serial, Other };
    Kind kind = Kind::DriveAdded;
    std::wstring root;       // a drive: "E:\" (also its identity); a phone: its interface path
    std::wstring name;       // "SanDisk", "USB drive", "Galaxy A52"
    std::wstring detail;     // a drive: "28.1 GB free · E:"
    Usb usb = Usb::Other;    // UsbAdded: what kind of device it is
};

class DeviceWatcher {
public:
    DeviceWatcher(HWND notify, UINT usbMessage);
    ~DeviceWatcher();
    DeviceWatcher(const DeviceWatcher&) = delete;
    DeviceWatcher& operator=(const DeviceWatcher&) = delete;

    std::vector<DeviceEvent> OnDeviceChange(WPARAM wp, LPARAM lp);
    static std::vector<DeviceEvent> CurrentDrives();          // removable drives plugged in now (at startup)

    // "Safely remove": runs on a worker thread, then posts `message` to `notify` with a heap-allocated EjectResult in lParam.
    struct EjectResult {
        std::wstring root, name;
        bool ok = false;
        std::wstring why;    // when not ok: "a file on it is still open"
    };
    static void Eject(const std::wstring& root, const std::wstring& name, HWND notify, UINT message);

private:
    HWND notify_ = nullptr;
    UINT usbMessage_ = 0;
    HDEVNOTIFY phones_ = nullptr, usb_ = nullptr;
    std::map<std::wstring, std::wstring> phoneNames_;         // interface path -> name (to announce removal)
};

} // namespace eflow
