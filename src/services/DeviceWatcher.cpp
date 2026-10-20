#include "services/DeviceWatcher.h"

#include <initguid.h>   // (first: the device-property keys and GUIDs below are defined here, not only declared)
#include <cfgmgr32.h>
#include <dbt.h>
#include <devpkey.h>
#include <setupapi.h>
#include <winioctl.h>

#include <cwchar>
#include <cwctype>
#include <optional>
#include <thread>

#include "core/Log.h"

// Portable devices (phones, cameras over MTP / PTP): {6AC27878-A6FA-4155-BA85-F98F491D4F33}
DEFINE_GUID(kWpdInterface, 0x6ac27878, 0xa6fa, 0x4155, 0xba, 0x85, 0xf9, 0x8f, 0x49, 0x1d, 0x4f, 0x33);
// Any USB device: {A5DCBF10-6530-11D2-901F-00C04FB951ED}
DEFINE_GUID(kUsbDeviceInterface, 0xa5dcbf10, 0x6530, 0x11d2, 0x90, 0x1f, 0x00, 0xc0, 0x4f, 0xb9, 0x51, 0xed);

namespace eflow {
namespace {

std::wstring FreeSpace(const std::wstring& root) {
    ULARGE_INTEGER free{};
    if (!GetDiskFreeSpaceExW(root.c_str(), &free, nullptr, nullptr)) return {};
    const double gb = static_cast<double>(free.QuadPart) / (1024.0 * 1024.0 * 1024.0);
    wchar_t buf[48];
    if (gb >= 10) swprintf_s(buf, L"%.0f GB free", gb);
    else if (gb >= 1) swprintf_s(buf, L"%.1f GB free", gb);
    else swprintf_s(buf, L"%.0f MB free", gb * 1024.0);
    return buf;
}

bool IsUsbDisk(const std::wstring& root) {                    // an external hard drive / SSD shows up as "fixed": ask the bus
    const std::wstring device = L"\\\\.\\" + root.substr(0, 2);
    HANDLE h = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    STORAGE_PROPERTY_QUERY query{StorageDeviceProperty, PropertyStandardQuery, {0}};
    BYTE buffer[1024]{};
    DWORD got = 0;
    bool usb = false;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer, sizeof(buffer), &got, nullptr))
        usb = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buffer)->BusType == BusTypeUsb;
    CloseHandle(h);
    return usb;
}

bool Removable(const std::wstring& root) {
    const UINT type = GetDriveTypeW(root.c_str());
    return type == DRIVE_REMOVABLE || (type == DRIVE_FIXED && IsUsbDisk(root));
}

DeviceEvent Drive(const std::wstring& root, DeviceEvent::Kind kind) {
    DeviceEvent e;
    e.kind = kind;
    e.root = root;
    wchar_t label[MAX_PATH + 1]{};
    if (kind == DeviceEvent::Kind::DriveAdded) GetVolumeInformationW(root.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
    e.name = label[0] ? label : L"USB drive";
    const std::wstring space = kind == DeviceEvent::Kind::DriveAdded ? FreeSpace(root) : L"";
    e.detail = space.empty() ? root.substr(0, 2) : space + L"  ·  " + root.substr(0, 2);
    return e;
}

std::wstring DevNodeName(DEVINST node);                                    // (below)

std::wstring DevNodeString(DEVINST node, const DEVPROPKEY& key) {
    DEVPROPTYPE type = 0;
    wchar_t buf[256]{};
    ULONG size = sizeof(buf);
    return CM_Get_DevNode_PropertyW(node, &key, &type, reinterpret_cast<PBYTE>(buf), &size, 0) == CR_SUCCESS ? buf : L"";
}

bool Contains(const std::wstring& text, const wchar_t* word) {
    std::wstring a = text, b = word;
    for (auto* s : {&a, &b}) for (wchar_t& c : *s) c = static_cast<wchar_t>(towlower(c));
    return a.find(b) != std::wstring::npos;
}

// What a USB device is, from Windows' device classes of the device and its functions (a headset is one USB device with
// audio and button functions). nullopt: not worth announcing (hubs; phones and headphones have their own pill).
std::optional<DeviceEvent> IdentifyUsb(const std::wstring& instance) {
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(instance.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return std::nullopt;
    std::vector<DEVINST> nodes{node};
    for (size_t i = 0; i < nodes.size() && nodes.size() < 24; ++i) {     // the device, its functions and theirs
        DEVINST child = 0;
        if (CM_Get_Child(&child, nodes[i], 0) != CR_SUCCESS) continue;
        do nodes.push_back(child); while (CM_Get_Sibling(&child, child, 0) == CR_SUCCESS);
    }
    using Usb = DeviceEvent::Usb;
    std::optional<Usb> kind;
    DEVINST named = node;
    auto pick = [&](Usb k, DEVINST n) { if (!kind) kind = k, named = n; };
    for (DEVINST n : nodes) {
        const std::wstring cls = DevNodeString(n, DEVPKEY_Device_Class), service = DevNodeString(n, DEVPKEY_Device_Service);
        const std::wstring name = DevNodeString(n, DEVPKEY_Device_FriendlyName) + L" " + DevNodeString(n, DEVPKEY_Device_DeviceDesc);
        if (Contains(service, L"usbhub") || Contains(name, L"usb hub") || Contains(name, L"root hub")) return std::nullopt;
        if (cls == L"WPD" || cls == L"MEDIA" || cls == L"AudioEndpoint") return std::nullopt;   // phones, audio: announced already
        if (cls == L"DiskDrive" || Contains(service, L"usbstor") || Contains(service, L"uaspstor")) pick(Usb::Storage, n);
        else if (cls == L"Net") pick(Contains(name, L"wireless") || Contains(name, L"wi-fi") || Contains(name, L"wifi") ||
                                       Contains(name, L"802.11") || Contains(name, L"wlan") ? Usb::WiFi : Usb::Network, n);
        else if (cls == L"Bluetooth") pick(Usb::Bluetooth, n);
        else if (cls == L"Keyboard") pick(Usb::Keyboard, n);
        else if (cls == L"Mouse") pick(Usb::Mouse, n);
        else if (cls == L"Camera" || cls == L"Image") pick(Usb::Camera, n);
        else if (cls == L"Printer") pick(Usb::Printer, n);
        else if (cls == L"Ports") pick(Usb::Serial, n);
    }
    if (!kind) {                                                           // only HID or unknown functions
        for (DEVINST n : nodes)
            if (DevNodeString(n, DEVPKEY_Device_Class) == L"HIDClass") { kind = Usb::Input; break; }
    }
    DeviceEvent e;
    e.kind = DeviceEvent::Kind::UsbAdded;
    e.usb = kind.value_or(Usb::Other);
    e.root = instance;
    e.name = DevNodeName(named);
    if (e.name.empty() || Contains(e.name, L"composite") || Contains(e.name, L"generic")) e.name = DevNodeName(node);
    if (Contains(e.name, L"composite")) e.name.clear();                  // "USB Composite Device" says nothing
    return e;
}

std::wstring DevNodeName(DEVINST node) {
    for (const DEVPROPKEY* key : {&DEVPKEY_Device_FriendlyName, &DEVPKEY_Device_DeviceDesc}) {
        DEVPROPTYPE type = 0;
        wchar_t buf[256]{};
        ULONG size = sizeof(buf);
        if (CM_Get_DevNode_PropertyW(node, key, &type, reinterpret_cast<PBYTE>(buf), &size, 0) == CR_SUCCESS && buf[0]) return buf;
    }
    return {};
}

}  // namespace

DeviceWatcher::DeviceWatcher(HWND notify, UINT usbMessage) : notify_(notify), usbMessage_(usbMessage) {
    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = kWpdInterface;
    phones_ = RegisterDeviceNotificationW(notify, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!phones_) Log("devices: phones will not be announced (%lu)", GetLastError());
    filter.dbcc_classguid = kUsbDeviceInterface;
    usb_ = RegisterDeviceNotificationW(notify, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!usb_) Log("devices: USB devices will not be announced (%lu)", GetLastError());
}

DeviceWatcher::~DeviceWatcher() {
    if (phones_) UnregisterDeviceNotification(phones_);
    if (usb_) UnregisterDeviceNotification(usb_);
}

std::vector<DeviceEvent> DeviceWatcher::CurrentDrives() {
    std::vector<DeviceEvent> out;
    const DWORD mask = GetLogicalDrives();
    for (int i = 2; i < 26; ++i) {                                     // (A: and B: are floppy letters)
        if (!(mask & (1u << i))) continue;
        const std::wstring root = std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\";
        if (Removable(root)) out.push_back(Drive(root, DeviceEvent::Kind::DriveAdded));
    }
    return out;
}

std::vector<DeviceEvent> DeviceWatcher::OnDeviceChange(WPARAM wp, LPARAM lp) {
    std::vector<DeviceEvent> out;
    if ((wp != DBT_DEVICEARRIVAL && wp != DBT_DEVICEREMOVECOMPLETE) || !lp) return out;
    const auto* header = reinterpret_cast<const DEV_BROADCAST_HDR*>(lp);
    const bool added = wp == DBT_DEVICEARRIVAL;
    if (header->dbch_devicetype == DBT_DEVTYP_VOLUME) {
        const auto* volume = reinterpret_cast<const DEV_BROADCAST_VOLUME*>(lp);
        if (volume->dbcv_flags & DBTF_NET) return out;                    // a network share, not something plugged in
        for (int i = 0; i < 26; ++i) {
            if (!(volume->dbcv_unitmask & (1u << i))) continue;
            const std::wstring root = std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\";
            if (added && !Removable(root)) continue;
            out.push_back(Drive(root, added ? DeviceEvent::Kind::DriveAdded : DeviceEvent::Kind::DriveRemoved));
        }
    } else if (header->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
        const auto* iface = reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(lp);
        const std::wstring path = iface->dbcc_name;
        if (IsEqualGUID(iface->dbcc_classguid, kUsbDeviceInterface)) {
            if (!added) return out;                                     // (unplugging needs no announcement)
            wchar_t instance[512]{};
            DEVPROPTYPE type = 0;
            ULONG size = sizeof(instance);
            if (CM_Get_Device_Interface_PropertyW(path.c_str(), &DEVPKEY_Device_InstanceId, &type, reinterpret_cast<PBYTE>(instance), &size, 0) != CR_SUCCESS)
                return out;
            std::thread([id = std::wstring(instance), notify = notify_, message = usbMessage_] {
                Sleep(1500);                                            // Windows sets up its functions (and drivers) first
                auto e = IdentifyUsb(id);
                if (!e) return;
                if (e->usb == DeviceEvent::Usb::Storage) Sleep(2500);   // (a drive with a letter announces itself: the app checks)
                auto* heap = new DeviceEvent(*e);
                if (!PostMessageW(notify, message, 0, reinterpret_cast<LPARAM>(heap))) delete heap;
            }).detach();
            return out;
        }
        if (!added) {
            if (auto it = phoneNames_.find(path); it != phoneNames_.end()) {
                out.push_back({DeviceEvent::Kind::PhoneRemoved, path, it->second, L""});
                phoneNames_.erase(it);
            }
            return out;
        }
        wchar_t instance[512]{};
        DEVPROPTYPE type = 0;
        ULONG size = sizeof(instance);
        if (CM_Get_Device_Interface_PropertyW(path.c_str(), &DEVPKEY_Device_InstanceId, &type, reinterpret_cast<PBYTE>(instance), &size, 0) != CR_SUCCESS)
            return out;
        if (_wcsnicmp(instance, L"USB\\", 4) != 0) return out;            // a USB stick also appears here: drives are announced above
        DEVINST node = 0;
        if (CM_Locate_DevNodeW(&node, instance, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return out;
        std::wstring name = DevNodeName(node);
        if (name.empty()) name = L"Phone";
        phoneNames_[path] = name;
        out.push_back({DeviceEvent::Kind::PhoneAdded, path, name, L""});
    }
    return out;
}

void DeviceWatcher::Eject(const std::wstring& root, const std::wstring& name, HWND notify, UINT message) {
    std::thread([root, name, notify, message] {
        auto* result = new EjectResult{root, name, false, L""};
        // The drive letter -> its disk number -> the disk's device -> its parent (the USB device) -> eject that.
        STORAGE_DEVICE_NUMBER wanted{};
        DWORD got = 0;
        const std::wstring device = L"\\\\.\\" + root.substr(0, 2);
        HANDLE h = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        const bool numbered = h != INVALID_HANDLE_VALUE &&
                              DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &wanted, sizeof(wanted), &got, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        DEVINST disk = 0;
        if (numbered) {
            HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISK, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            SP_DEVICE_INTERFACE_DATA data{sizeof(data)};
            for (DWORD i = 0; !disk && SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_DEVINTERFACE_DISK, i, &data); ++i) {
                DWORD need = 0;
                SetupDiGetDeviceInterfaceDetailW(set, &data, nullptr, 0, &need, nullptr);
                std::vector<BYTE> buffer(need);
                auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
                detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                SP_DEVINFO_DATA info{sizeof(info)};
                if (!SetupDiGetDeviceInterfaceDetailW(set, &data, detail, need, nullptr, &info)) continue;
                HANDLE d = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                if (d == INVALID_HANDLE_VALUE) continue;
                STORAGE_DEVICE_NUMBER n{};
                if (DeviceIoControl(d, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &n, sizeof(n), &got, nullptr) &&
                    n.DeviceNumber == wanted.DeviceNumber && n.DeviceType == wanted.DeviceType)
                    disk = info.DevInst;
                CloseHandle(d);
            }
            SetupDiDestroyDeviceInfoList(set);
        }
        DEVINST parent = 0;
        if (!disk || CM_Get_Parent(&parent, disk, 0) != CR_SUCCESS) {
            result->why = L"Windows did not let Eflow find this drive";
        } else {
            PNP_VETO_TYPE veto = PNP_VetoTypeUnknown;
            wchar_t vetoName[MAX_PATH]{};
            for (int attempt = 0; attempt < 3 && !result->ok; ++attempt) {   // Explorer often lets go a moment later
                veto = PNP_VetoTypeUnknown;
                result->ok = CM_Request_Device_EjectW(parent, &veto, vetoName, MAX_PATH, 0) == CR_SUCCESS && veto == PNP_VetoTypeUnknown;
                if (!result->ok) Sleep(400);
            }
            if (!result->ok)
                result->why = veto == PNP_VetoOutstandingOpen ? L"a file on it is still open" : L"it is still in use";
        }
        if (!PostMessageW(notify, message, 0, reinterpret_cast<LPARAM>(result))) delete result;
    }).detach();
}

} // namespace eflow
