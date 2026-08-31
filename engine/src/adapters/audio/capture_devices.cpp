#include "adapters/audio/capture_devices.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <mmdeviceapi.h>
#include <propsys.h>
#include <windows.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace ambient::audio {

namespace {

using Microsoft::WRL::ComPtr;

// DEVPKEY_Device_* keys spelled out to avoid the SDK initguid clash
constexpr GUID kDeviceGuid{
    0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}};
constexpr PROPERTYKEY kFriendlyName{kDeviceGuid, 14};    // endpoint (adapter)
constexpr PROPERTYKEY kDeviceDesc{kDeviceGuid, 2};       // endpoint alone
constexpr PROPERTYKEY kEnumeratorName{kDeviceGuid, 24};  // "BTHENUM" is Bluetooth

struct ComApartment {
    HRESULT hr;
    ComApartment() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
};

std::string Utf8(const wchar_t* wide) {
    if (wide == nullptr || *wide == L'\0') return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(static_cast<std::size_t>(size) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), size, nullptr, nullptr);
    return out;
}

std::string Property(IPropertyStore* store, const PROPERTYKEY& key) {
    PROPVARIANT value;
    PropVariantInit(&value);
    std::string out;
    if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_LPWSTR) {
        out = Utf8(value.pwszVal);
    }
    PropVariantClear(&value);
    return out;
}

std::wstring EndpointId(IMMDevice* device) {
    wchar_t* id = nullptr;
    if (FAILED(device->GetId(&id))) return {};
    std::wstring out(id);
    CoTaskMemFree(id);
    return out;
}

}  // namespace

std::vector<CaptureDevice> ListCaptureDevices() {
    std::vector<CaptureDevice> devices;
    const ComApartment com;
    if (FAILED(com.hr)) return devices;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) {
        return devices;
    }

    // The same default WasapiCapture resolves when no id is pinned
    std::wstring default_id;
    ComPtr<IMMDevice> default_device;
    if (SUCCEEDED(
            enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &default_device))) {
        default_id = EndpointId(default_device.Get());
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection))) {
        return devices;
    }
    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) return devices;

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;
        const std::wstring id = EndpointId(device.Get());
        if (id.empty()) continue;

        ComPtr<IPropertyStore> store;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) continue;

        CaptureDevice entry;
        entry.id = Utf8(id.c_str());
        entry.name = Property(store.Get(), kFriendlyName);
        entry.short_name = Property(store.Get(), kDeviceDesc);
        if (entry.name.empty()) entry.name = entry.short_name;
        if (entry.name.empty()) entry.name = "Microphone";
        if (entry.short_name.empty()) entry.short_name = entry.name;
        entry.is_default = !default_id.empty() && id == default_id;
        // BTHENUM is classic Bluetooth, BTHLE is LE Audio (measured: the
        // WF-1000XM6 enumerates as BTHLE); the prefix covers the family
        entry.bluetooth = Property(store.Get(), kEnumeratorName).starts_with("BTH");
        devices.push_back(std::move(entry));
    }
    return devices;
}

CaptureDevice ResolveMicrophone(const std::vector<CaptureDevice>& devices,
                                const std::string& requested) {
    if (!requested.empty()) {
        for (const auto& device : devices) {
            if (device.id == requested) return device;
        }
    }
    for (const auto& device : devices) {
        if (device.is_default) return device;
    }
    return devices.empty() ? CaptureDevice{} : devices.front();
}

std::wstring WideId(const std::string& id) {
    if (id.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, id.c_str(), -1, nullptr, 0);
    if (size <= 1) return {};
    std::wstring out(static_cast<std::size_t>(size) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, id.c_str(), -1, out.data(), size);
    return out;
}

}  // namespace ambient::audio
