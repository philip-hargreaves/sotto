#include "adapters/audio/wasapi_player.hpp"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "ports/audio_source.hpp"

namespace ambient::audio {

namespace {

struct ComApartment {
    HRESULT hr;
    ComApartment() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
};

}  // namespace

// The apartment is declared first so it outlives every COM pointer; the
// replay thread never initialises COM itself
struct WasapiPlayer::Impl {
    ComApartment com;
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    Microsoft::WRL::ComPtr<IMMDevice> device;
    Microsoft::WRL::ComPtr<IAudioClient> client;
    Microsoft::WRL::ComPtr<IAudioRenderClient> render;
    UINT32 buffer_frames = 0;
    unsigned long long written = 0;
    unsigned long long dropped = 0;

    bool Open() {
        if (FAILED(com.hr)) return false;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator)))) {
            return false;
        }
        // eConsole: Windows ducks the communications role during calls
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) || !device) {
            return false;
        }
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
            return false;
        }
        WAVEFORMATEX want{};
        want.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        want.nChannels = 1;
        want.nSamplesPerSec = kSampleRate;
        want.wBitsPerSample = 32;
        want.nBlockAlign = static_cast<WORD>(want.nChannels * want.wBitsPerSample / 8);
        want.nAvgBytesPerSec = want.nSamplesPerSec * want.nBlockAlign;
        constexpr REFERENCE_TIME kBuffer = 4'000'000;  // 400 ms; drops are free
        const DWORD flags =
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (FAILED(
                client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBuffer, 0, &want, nullptr))) {
            return false;
        }
        if (FAILED(client->GetBufferSize(&buffer_frames))) return false;
        if (FAILED(client->GetService(IID_PPV_ARGS(&render)))) return false;

        // Pre-roll half a buffer of silence: the caller writes at the drain
        // rate, so without a cushion every scheduling hiccup is audible
        const UINT32 pre = buffer_frames / 2;
        BYTE* silence = nullptr;
        if (SUCCEEDED(render->GetBuffer(pre, &silence)) && silence != nullptr) {
            std::memset(silence, 0, static_cast<std::size_t>(pre) * sizeof(float));
            render->ReleaseBuffer(pre, 0);
        }
        return SUCCEEDED(client->Start());
    }
};

std::unique_ptr<WasapiPlayer> WasapiPlayer::Open() {
    auto impl = std::make_unique<Impl>();
    if (!impl->Open()) {
        std::fprintf(stderr, "ambient-engine: no render endpoint - replay is silent\n");
        return nullptr;
    }
    return std::unique_ptr<WasapiPlayer>(new WasapiPlayer(std::move(impl)));
}

WasapiPlayer::WasapiPlayer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

WasapiPlayer::~WasapiPlayer() {
    // Logged so silent playback is diagnosable
    std::fprintf(stderr, "ambient-engine: monitor played %llu frames, dropped %llu\n",
                 impl_->written, impl_->dropped);
    if (impl_->client) impl_->client->Stop();
}

void WasapiPlayer::Write(std::span<const float> frames) {
    auto& s = *impl_;
    if (!s.render || !s.client || frames.empty()) return;
    UINT32 padding = 0;
    if (FAILED(s.client->GetCurrentPadding(&padding))) return;
    const UINT32 room = s.buffer_frames > padding ? s.buffer_frames - padding : 0;
    if (room == 0) {
        s.dropped += frames.size();
        return;
    }
    const auto take = static_cast<UINT32>(std::min<std::size_t>(frames.size(), room));
    BYTE* dst = nullptr;
    if (FAILED(s.render->GetBuffer(take, &dst)) || dst == nullptr) return;
    std::memcpy(dst, frames.data(), static_cast<std::size_t>(take) * sizeof(float));
    s.render->ReleaseBuffer(take, 0);
    s.written += take;
    if (frames.size() > take) s.dropped += frames.size() - take;
}

}  // namespace ambient::audio
