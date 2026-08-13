#include "adapters/audio/wasapi_capture.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off: appmodel.h needs windows.h first
#include <windows.h>
#include <appmodel.h>
// clang-format on
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "adapters/audio/capture_errors.hpp"
#include "adapters/audio/capture_timeline.hpp"

namespace sotto::audio {

namespace {

using Microsoft::WRL::ComPtr;

constexpr DWORD kStreamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                               AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                               AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

SourceEnd Fail(const char* what, HRESULT hr) {
    return EndForCaptureError(what, static_cast<std::uint32_t>(hr));
}

std::wstring ConsentStoreValue(const std::wstring& subkey) {
    wchar_t value[16]{};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, subkey.c_str(), L"Value", RRF_RT_REG_SZ, nullptr, value,
                     &size) != ERROR_SUCCESS) {
        return L"";
    }
    return value;
}

// Windows records the Settings microphone toggle but does not enforce it
// against a full-trust process, and CheckAccess reports the unenforced
// truth: Allowed even under an explicit deny (both measured). The store the
// Settings page writes is therefore the only signal of what the user chose,
// so a clinical recorder reads it and enforces it itself.
bool ConsentDenied() {
    const std::wstring store =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore"
        L"\\microphone";
    if (ConsentStoreValue(store) == L"Deny") {
        return true;
    }

    wchar_t family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1]{};
    UINT32 length = PACKAGE_FAMILY_NAME_MAX_LENGTH + 1;
    if (GetCurrentPackageFamilyName(&length, family) == ERROR_SUCCESS) {
        return ConsentStoreValue(store + L"\\" + family) == L"Deny";
    }
    return ConsentStoreValue(store + L"\\NonPackaged") == L"Deny";
}

struct ComApartment {
    HRESULT hr;
    ComApartment() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
};

struct OwnedHandle {
    HANDLE handle = nullptr;
    ~OwnedHandle() {
        if (handle != nullptr) CloseHandle(handle);
    }
};

struct StopOnExit {
    IAudioClient* client;
    ~StopOnExit() {
        client->Stop();
    }
};

}  // namespace

WasapiCapture::WasapiCapture(std::wstring endpoint_id) : endpoint_id_(std::move(endpoint_id)) {
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

WasapiCapture::~WasapiCapture() {
    if (stop_event_ != nullptr) {
        CloseHandle(stop_event_);
    }
}

void WasapiCapture::RequestStop() {
    if (stop_event_ != nullptr) {
        SetEvent(stop_event_);
    }
}

// OnEnd is the port's one guarantee, so Run funnels every outcome through it
void WasapiCapture::Run(IAudioSink& sink) {
    sink.OnEnd(RunToEnd(sink));
}

SourceEnd WasapiCapture::RunToEnd(IAudioSink& sink) {
    if (stop_event_ == nullptr) {
        return {SourceEndReason::kFailed, "stop event could not be created"};
    }

    const ComApartment com;
    if (FAILED(com.hr)) {
        return Fail("CoInitializeEx", com.hr);
    }

    if (ConsentDenied()) {
        return {SourceEndReason::kFailed, "microphone access denied in Windows privacy settings"};
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        return Fail("CoCreateInstance", hr);
    }

    // Resolve once; from here the device is pinned for the whole stream
    ComPtr<IMMDevice> device;
    hr = endpoint_id_.empty()
             ? enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &device)
             : enumerator->GetDevice(endpoint_id_.c_str(), &device);
    if (FAILED(hr)) {
        return Fail(endpoint_id_.empty() ? "GetDefaultAudioEndpoint" : "GetDevice", hr);
    }

    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (FAILED(hr)) {
        return Fail("Activate", hr);
    }

    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr)) {
        return Fail("GetMixFormat", hr);
    }
    const std::uint32_t native_rate = mix->nSamplesPerSec;
    CoTaskMemFree(mix);

    WAVEFORMATEX wanted{};
    wanted.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wanted.nChannels = 1;
    wanted.nSamplesPerSec = static_cast<DWORD>(kSampleRate);
    wanted.wBitsPerSample = 32;
    wanted.nBlockAlign = 4;
    wanted.nAvgBytesPerSec = wanted.nSamplesPerSec * wanted.nBlockAlign;

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, kStreamFlags, 0, 0, &wanted, nullptr);
    if (FAILED(hr)) {
        return Fail("Initialize", hr);
    }

    OwnedHandle audio_event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    if (audio_event.handle == nullptr) {
        return Fail("CreateEventW", HRESULT_FROM_WIN32(GetLastError()));
    }
    hr = client->SetEventHandle(audio_event.handle);
    if (FAILED(hr)) {
        return Fail("SetEventHandle", hr);
    }

    ComPtr<IAudioCaptureClient> capture;
    hr = client->GetService(IID_PPV_ARGS(&capture));
    if (FAILED(hr)) {
        return Fail("GetService", hr);
    }

    UINT32 buffer_frames = 0;
    hr = client->GetBufferSize(&buffer_frames);
    if (FAILED(hr)) {
        return Fail("GetBufferSize", hr);
    }
    std::vector<float> packet(buffer_frames);

    hr = client->Start();
    if (FAILED(hr)) {
        return Fail("Start", hr);
    }
    const StopOnExit stopper{client.Get()};

    CaptureTimeline timeline(native_rate);
    const HANDLE waits[2] = {static_cast<HANDLE>(stop_event_), audio_event.handle};
    for (;;) {
        const DWORD wake = WaitForMultipleObjects(2, waits, FALSE, 2000);
        if (wake == WAIT_OBJECT_0) {
            return {SourceEndReason::kStopped, ""};
        }
        if (wake == WAIT_FAILED) {
            return Fail("WaitForMultipleObjects", HRESULT_FROM_WIN32(GetLastError()));
        }
        // On a timeout fall through to the drain: a dead device stops
        // signalling, and only a capture call reports what happened to it

        for (;;) {
            UINT32 next = 0;
            hr = capture->GetNextPacketSize(&next);
            if (FAILED(hr)) {
                return Fail("GetNextPacketSize", hr);
            }
            if (next == 0) {
                break;
            }

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 position = 0;
            UINT64 qpc = 0;
            hr = capture->GetBuffer(&data, &frames, &flags, &position, &qpc);
            if (FAILED(hr)) {
                return Fail("GetBuffer", hr);
            }

            const std::uint64_t lost = timeline.OnPacket(
                position, frames, (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0);
            if (frames > packet.size()) {
                packet.resize(frames);
            }
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
                std::memset(packet.data(), 0, frames * sizeof(float));
            } else {
                std::memcpy(packet.data(), data, frames * sizeof(float));
            }

            // Release inside the buffer period; the sink runs after, on our copy
            hr = capture->ReleaseBuffer(frames);
            if (FAILED(hr)) {
                return Fail("ReleaseBuffer", hr);
            }
            sink.OnAudio(std::span<const float>(packet.data(), frames), lost);
        }
    }
}

}  // namespace sotto::audio
