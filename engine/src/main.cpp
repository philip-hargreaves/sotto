#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#ifdef _DEBUG
#include <crtdbg.h>
#endif

#include "adapters/audio/wasapi_capture.hpp"
#include "adapters/audio/wav_source.hpp"
#include "adapters/diarisation/deferred_diariser.hpp"
#include "adapters/diarisation/speaker_diariser.hpp"
#include "adapters/ipc/handlers.hpp"
#include "adapters/ipc/pipe_server.hpp"
#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "adapters/note/qwen_note_writer.hpp"
#include "adapters/note/worker_note_writer.hpp"
#include "adapters/storage/sqlite_session_store.hpp"
#include "adapters/transcription/scripted_transcriber.hpp"
#include "adapters/transcription/whisper_transcriber.hpp"
#include "adapters/translate/nllb_translator.hpp"
#include "adapters/translate/translate_lane.hpp"
#include "adapters/vad/deferred_vad.hpp"
#include "adapters/vad/passthrough_vad.hpp"
#include "adapters/vad/silero_vad.hpp"
#include "core/cli_args.hpp"
#include "core/metrics.hpp"
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

    // Partials are cumulative, so intermediates can drop freely: a ~12 Hz
    // cap keeps the pipe and the shell's bindings calm at token cadence
    void PushPartial(const std::string& method, nlohmann::json params) {
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(throttle_mutex_);
            auto& last = last_partial_[method];
            if (now - last < std::chrono::milliseconds(80)) {
                return;
            }
            last = now;
        }
        server_.PushNotification(method, std::move(params));
    }

    void OnNotePartial(const std::string& text) override {
        PushPartial("note/partial", {{"text", text}});
    }

    void OnNoteReady(const std::string& text) override {
        server_.PushNotification("note/ready", {{"text", text}});
        // The translator warms while the patient sheet writes, so the first
        // translation is as fast as the rest
        if (translator_ != nullptr) {
            translator_->Prepare();
        }
    }

    void SetTranslator(sotto::translate::ITranslator* translator) {
        translator_ = translator;
    }

    void OnNoteFailed(const std::string& detail) override {
        server_.PushNotification("note/failed", {{"detail", detail}});
    }

    void OnPatientPartial(const std::string& text) override {
        PushPartial("patient/partial", {{"text", text}});
    }

    void OnPatientReady(const std::string& text) override {
        server_.PushNotification("patient/ready", {{"text", text}});
    }

    void OnPatientFailed(const std::string& detail) override {
        server_.PushNotification("patient/failed", {{"detail", detail}});
    }

   private:
    static const char* ReasonName(sotto::audio::SourceEndReason reason) {
        return reason == sotto::audio::SourceEndReason::kDeviceLost ? "deviceLost" : "failed";
    }

    sotto::ipc::PipeServer& server_;
    sotto::translate::ITranslator* translator_ = nullptr;
    std::mutex throttle_mutex_;
    std::map<std::string, std::chrono::steady_clock::time_point> last_partial_;
};

}  // namespace

int main(int argc, char* argv[]) {
#ifdef _DEBUG
    // A debug-CRT assert must reach stderr and abort, never hang the
    // headless engine behind a modal dialog
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
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
                    replay->path, sotto::audio::WavSource::Config{replay->speed, replay->monitor,
                                                                  replay->start_frame});
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
        sotto::metrics::Registry metrics;
        std::unique_ptr<sotto::asr::ITranscriber> transcriber;
        try {
            model_store.Resolve("asr", "default");
            transcriber = std::make_unique<sotto::asr::WhisperTranscriber>(model_store, ov_runtime,
                                                                           asr_device, &metrics);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "sotto-engine: scripted transcripts (%s)\n", e.what());
            transcriber = std::make_unique<sotto::asr::ScriptedTranscriber>();
        }
        // Same pattern for the VAD: real endpointing when staged. Compiles
        // behind the serve loop; session/start waits on it, hello does not
        std::unique_ptr<sotto::audio::IStreamingVad> vad;
        try {
            model_store.Resolve("vad", "default");
            vad = std::make_unique<sotto::audio::DeferredVad>([&model_store, &ov_runtime,
                                                               &metrics] {
                const auto t0 = std::chrono::steady_clock::now();
                auto built = std::make_unique<sotto::audio::SileroVad>(model_store, ov_runtime);
                metrics.RecordLoad(
                    "vad",
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
                return built;
            });
        } catch (const std::exception& e) {
            std::fprintf(stderr, "sotto-engine: capped windows (%s)\n", e.what());
            vad = std::make_unique<sotto::audio::PassthroughVad>();
        }
        // Diarisation needs both its models; without them turns simply keep
        // an empty speaker
        std::unique_ptr<sotto::diar::IDiariser> diariser;
        try {
            model_store.Resolve("diarisation", "default");
            model_store.Resolve("segmentation", "default");
            diariser = std::make_unique<sotto::diar::DeferredDiariser>([&model_store, &ov_runtime,
                                                                        store_root, &metrics] {
                const auto t0 = std::chrono::steady_clock::now();
                auto built = std::make_unique<sotto::diar::SpeakerDiariser>(model_store, ov_runtime,
                                                                            store_root);
                metrics.RecordLoad(
                    "diarisation",
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
                return built;
            });
        } catch (const std::exception& e) {
            std::fprintf(stderr, "sotto-engine: no speaker labels (%s)\n", e.what());
        }
        // The prompt lives beside the models dir so edits apply per note.
        // Generation runs in its own supervised process: a GPU driver fault
        // there costs a respawn, never the engine (measured; see ADR-0027)
        std::unique_ptr<sotto::note::INoteWriter> note_writer;
        try {
            model_store.Resolve("note", "default");
            const auto prompt = models_root.parent_path() / "prompts" / "note-narrative.md";
            wchar_t exe_path[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
            const auto host = std::filesystem::path(exe_path).parent_path() / "sotto_note_host.exe";
            if (std::filesystem::exists(host)) {
                note_writer =
                    std::make_unique<sotto::note::WorkerNoteWriter>(host, models_root, prompt);
            } else {
                std::fprintf(stderr, "sotto-engine: note host missing, writing in-process\n");
                note_writer = std::make_unique<sotto::note::QwenNoteWriter>(model_store, ov_runtime,
                                                                            prompt, &metrics);
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "sotto-engine: stub note (%s)\n", e.what());
        }
        // Translation runs on the CPU, so it never contends with the GPU
        std::unique_ptr<sotto::translate::NllbTranslator> translator;
        std::unique_ptr<sotto::translate::TranslateLane> translate_lane;
        try {
            model_store.Resolve("translation", "default");
            translator =
                std::make_unique<sotto::translate::NllbTranslator>(model_store, ov_runtime);
            translate_lane = std::make_unique<sotto::translate::TranslateLane>(
                *translator,
                [&server, &events](const std::string& method, const nlohmann::json& params) {
                    if (method == "translate/partial") {
                        events.PushPartial(method, params);
                    } else {
                        server.PushNotification(method, params);
                    }
                });
            events.SetTranslator(translator.get());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "sotto-engine: no translation (%s)\n", e.what());
        }
        sotto::audio::SessionController controller(
            std::move(factory), events, session_store, *transcriber, *vad, std::chrono::seconds(3),
            diariser.get(), 5 * sotto::audio::kSampleRate, note_writer.get(), &metrics);

        sotto::ipc::RegisterMethods(server, controller, model_store, session_store, &metrics,
                                    &ov_runtime, translator.get(), translate_lane.get());
        server.ServeOneClient();
        controller.Stop();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sotto-engine: %s\n", e.what());
        return 1;
    }
}
