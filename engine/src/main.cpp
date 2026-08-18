#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "adapters/audio/wasapi_capture.hpp"
#include "adapters/audio/wav_source.hpp"
#include "adapters/diarisation/speaker_diariser.hpp"
#include "adapters/ipc/handlers.hpp"
#include "adapters/ipc/pipe_server.hpp"
#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "adapters/storage/sqlite_session_store.hpp"
#include "adapters/transcription/scripted_transcriber.hpp"
#include "adapters/transcription/whisper_transcriber.hpp"
#include "adapters/vad/passthrough_vad.hpp"
#include "adapters/vad/silero_vad.hpp"
#include "core/session_controller.hpp"

namespace {

class WireEvents : public sotto::audio::ISessionEvents {
   public:
    explicit WireEvents(sotto::ipc::PipeServer& server) : server_(server) {}

    void OnLevel(const sotto::audio::LevelReading& reading) override {
        server_.PushNotification("audio.level",
                                 {{"level", reading.level}, {"clipped", reading.clipped}});
    }

    void OnTurn(const sotto::asr::Turn& turn) override {
        server_.PushNotification("transcript.turn", {{"firstFrame", turn.first_frame},
                                                     {"frameCount", turn.frame_count},
                                                     {"speaker", turn.speaker},
                                                     {"text", turn.text}});
    }

    void OnInterrupted(sotto::audio::SourceEndReason reason, const std::string& detail) override {
        server_.PushNotification("session/interrupted",
                                 {{"reason", ReasonName(reason)}, {"detail", detail}});
    }

   private:
    static const char* ReasonName(sotto::audio::SourceEndReason reason) {
        return reason == sotto::audio::SourceEndReason::kDeviceLost ? "deviceLost" : "failed";
    }

    sotto::ipc::PipeServer& server_;
};

}  // namespace

int main(int argc, char* argv[]) {
    try {
        // argv: [1] private pipe name, [2] store root, [3] models root,
        // [4] replay wav path; tests pass their own so runs collide with
        // neither the app nor its stores
        std::wstring pipe_name = L"\\\\.\\pipe\\LOCAL\\sotto-engine";
        if (argc > 1) {
            pipe_name = L"\\\\.\\pipe\\" + std::wstring(argv[1], argv[1] + std::strlen(argv[1]));
        }

        std::filesystem::path store_root;
        if (argc > 2) {
            store_root = argv[2];
        } else {
            char* local_app_data = nullptr;
            if (_dupenv_s(&local_app_data, nullptr, "LOCALAPPDATA") != 0 ||
                local_app_data == nullptr) {
                throw std::runtime_error("LOCALAPPDATA is not set and no store root was given");
            }
            store_root = std::filesystem::path(local_app_data) / "sotto" / "store";
            std::free(local_app_data);
        }

        // Beside the executable is the production shape: the MSIX package dir
        std::filesystem::path models_root;
        if (argc > 3) {
            models_root = argv[3];
        } else {
            wchar_t exe_path[MAX_PATH];
            GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
            const auto exe_dir = std::filesystem::path(exe_path).parent_path();
            models_root = exe_dir / "models";
            // Packaged debug runs put the exe one level below the layout
            if (!std::filesystem::exists(models_root)) {
                models_root = exe_dir.parent_path() / "models";
            }
        }

        sotto::ipc::PipeServer server(pipe_name);
        WireEvents events(server);
        sotto::store::SqliteSessionStore session_store(store_root);
        sotto::models::ModelStore model_store(models_root);

        // A replay request plays a wav through the same port; a launch-time
        // wav path (CI, scripts) forces every session to replay that file
        sotto::audio::SourceFactory factory =
            [forced = argc > 4 ? std::string(argv[4]) : std::string()](
                const std::optional<sotto::audio::ReplaySpec>& replay)
            -> std::unique_ptr<sotto::audio::IAudioSource> {
            if (replay.has_value()) {
                return std::make_unique<sotto::audio::WavSource>(
                    replay->path, sotto::audio::WavSource::Config{replay->speed, replay->monitor});
            }
            if (!forced.empty()) {
                return std::make_unique<sotto::audio::WavSource>(forced);
            }
            return std::make_unique<sotto::audio::WasapiCapture>();
        };
        // The store's contents decide: real transcription when the ASR role
        // is staged, scripted otherwise (CI, fresh installs). Whisper loads
        // on its worker thread, so the engine serves and records immediately
        // and queued windows decode once the model is ready
        sotto::models::OvRuntime ov_runtime;
        std::unique_ptr<sotto::asr::ITranscriber> transcriber;
        try {
            model_store.Resolve("asr", "default");
            transcriber = std::make_unique<sotto::asr::WhisperTranscriber>(model_store, ov_runtime);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "sotto-engine: scripted transcripts (%s)\n", e.what());
            transcriber = std::make_unique<sotto::asr::ScriptedTranscriber>();
        }
        // Same pattern for the VAD: real endpointing when staged
        std::unique_ptr<sotto::audio::IStreamingVad> vad;
        try {
            model_store.Resolve("vad", "default");
            vad = std::make_unique<sotto::audio::SileroVad>(model_store, ov_runtime);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "sotto-engine: capped windows (%s)\n", e.what());
            vad = std::make_unique<sotto::audio::PassthroughVad>();
        }
        // Diarisation needs both its models; without them turns simply keep
        // an empty speaker
        std::unique_ptr<sotto::diar::SpeakerDiariser> diariser;
        try {
            model_store.Resolve("diarisation", "default");
            model_store.Resolve("segmentation", "default");
            diariser =
                std::make_unique<sotto::diar::SpeakerDiariser>(model_store, ov_runtime, store_root);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "sotto-engine: no speaker labels (%s)\n", e.what());
        }
        sotto::audio::SessionController controller(std::move(factory), events, session_store,
                                                   *transcriber, *vad, std::chrono::seconds(3),
                                                   diariser.get());

        sotto::ipc::RegisterMethods(server, controller, model_store, session_store);
        server.ServeOneClient();
        controller.Stop();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sotto-engine: %s\n", e.what());
        return 1;
    }
}
