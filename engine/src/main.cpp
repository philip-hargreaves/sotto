#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

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
#include "adapters/note/qwen_note_writer.hpp"
#include "adapters/storage/sqlite_session_store.hpp"
#include "adapters/transcription/scripted_transcriber.hpp"
#include "adapters/transcription/whisper_transcriber.hpp"
#include "adapters/vad/passthrough_vad.hpp"
#include "adapters/vad/silero_vad.hpp"
#include "core/cli_args.hpp"
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

    void OnNotePartial(const std::string& text) override {
        server_.PushNotification("note/partial", {{"text", text}});
    }

    void OnNoteReady(const std::string& text) override {
        server_.PushNotification("note/ready", {{"text", text}});
    }

    void OnNoteFailed(const std::string& detail) override {
        server_.PushNotification("note/failed", {{"detail", detail}});
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
        // Flags first, then positional: pipe name, store root, models root,
        // replay wav; tests pass their own so runs collide with neither the
        // app nor its stores
        std::vector<std::string> args(argv + 1, argv + argc);
        const std::string asr_device = sotto::TakeFlag(args, "--asr-device");

        std::wstring pipe_name = L"\\\\.\\pipe\\LOCAL\\sotto-engine";
        if (args.size() > 0) {
            pipe_name = L"\\\\.\\pipe\\" + std::wstring(args[0].begin(), args[0].end());
        }

        std::filesystem::path store_root;
        if (args.size() > 1) {
            store_root = args[1];
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
        if (args.size() > 2) {
            models_root = args[2];
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
            [forced = args.size() > 3 ? args[3] : std::string()](
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
            transcriber = std::make_unique<sotto::asr::WhisperTranscriber>(model_store, ov_runtime,
                                                                           asr_device);
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
        // The prompt lives beside the models dir so edits apply per note
        std::unique_ptr<sotto::note::QwenNoteWriter> note_writer;
        try {
            model_store.Resolve("note", "default");
            note_writer = std::make_unique<sotto::note::QwenNoteWriter>(
                model_store, ov_runtime,
                models_root.parent_path() / "prompts" / "note-narrative.md");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "sotto-engine: stub note (%s)\n", e.what());
        }
        sotto::audio::SessionController controller(
            std::move(factory), events, session_store, *transcriber, *vad, std::chrono::seconds(3),
            diariser.get(), 5 * sotto::audio::kSampleRate, note_writer.get());

        sotto::ipc::RegisterMethods(server, controller, model_store, session_store);
        server.ServeOneClient();
        controller.Stop();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sotto-engine: %s\n", e.what());
        return 1;
    }
}
