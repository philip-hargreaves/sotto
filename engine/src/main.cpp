#include <chrono>
#include <cmath>
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

#include "adapters/audio/capture_devices.hpp"
#include "adapters/audio/wasapi_capture.hpp"
#include "adapters/audio/wav_source.hpp"
#include "adapters/diarisation/deferred_diariser.hpp"
#include "adapters/diarisation/speaker_diariser.hpp"
#include "adapters/host/power_throttling.hpp"
#include "adapters/ipc/handlers.hpp"
#include "adapters/ipc/pipe_server.hpp"
#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"
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
#include "core/throughput.hpp"

namespace {

class WireEvents : public ambient::audio::ISessionEvents {
   public:
    explicit WireEvents(ambient::ipc::PipeServer& server) : server_(server) {}

    void OnLevel(const ambient::audio::LevelReading& reading) override {
        server_.PushNotification("audio.level",
                                 {{"level", reading.level}, {"clipped", reading.clipped}});
    }

    void OnTurn(const ambient::asr::Turn& turn) override {
        server_.PushNotification("transcript.turn", {{"firstFrame", turn.first_frame},
                                                     {"frameCount", turn.frame_count},
                                                     {"speaker", turn.speaker},
                                                     {"text", turn.text}});
    }

    void OnInterrupted(ambient::audio::SourceEndReason reason, const std::string& detail) override {
        server_.PushNotification("session/interrupted",
                                 {{"reason", ReasonName(reason)}, {"detail", detail}});
    }

    void OnProgress(const std::string& stage) override {
        server_.PushNotification("session/progress", {{"stage", stage}});
    }

    // Metered before the ~12 Hz notification cap, so tokensPerSecond is the
    // model's real rate (Intel-requested figure)
    void PushPartial(const std::string& method, nlohmann::json params) {
        const auto now = std::chrono::steady_clock::now();
        double rate = 0;
        {
            std::lock_guard<std::mutex> lock(throttle_mutex_);
            auto& meter = meters_[method];
            meter.Token(Seconds(now));
            rate = meter.Rate(Seconds(now));
            auto& last = last_partial_[method];
            if (now - last < std::chrono::milliseconds(80)) {
                return;
            }
            last = now;
        }
        params["tokensPerSecond"] = Rounded(rate);
        server_.PushNotification(method, std::move(params));
    }

    // The end event carries the whole-generation average and retires the meter
    void PushStreamEnd(const char* partial_method, const char* method, nlohmann::json params) {
        double average = 0;
        {
            std::lock_guard<std::mutex> lock(throttle_mutex_);
            average = meters_[partial_method].Average();
            meters_.erase(partial_method);
            last_partial_.erase(partial_method);
        }
        if (average > 0) {
            params["tokensPerSecond"] = Rounded(average);
        }
        server_.PushNotification(method, std::move(params));
    }

    void DropStream(const char* partial_method) {
        std::lock_guard<std::mutex> lock(throttle_mutex_);
        meters_.erase(partial_method);
        last_partial_.erase(partial_method);
    }

    void OnNotePartial(const std::string& text) override {
        PushPartial("note/partial", {{"text", text}});
    }

    void OnNoteReady(const std::string& text) override {
        PushStreamEnd("note/partial", "note/ready", {{"text", text}});
        // The translator warms while the patient sheet writes, so the first
        // translation is as fast as the rest
        if (translator_ != nullptr) {
            translator_->Prepare();
        }
    }

    void SetTranslator(ambient::translate::ITranslator* translator) {
        translator_ = translator;
    }

    void OnNoteFailed(const std::string& detail) override {
        DropStream("note/partial");
        server_.PushNotification("note/failed", {{"detail", detail}});
    }

    void OnPatientPartial(const std::string& text) override {
        PushPartial("patient/partial", {{"text", text}});
    }

    void OnPatientReady(const std::string& text) override {
        PushStreamEnd("patient/partial", "patient/ready", {{"text", text}});
    }

    void OnPatientFailed(const std::string& detail) override {
        DropStream("patient/partial");
        server_.PushNotification("patient/failed", {{"detail", detail}});
    }

   private:
    static const char* ReasonName(ambient::audio::SourceEndReason reason) {
        return reason == ambient::audio::SourceEndReason::kDeviceLost ? "deviceLost" : "failed";
    }

    double Seconds(std::chrono::steady_clock::time_point now) const {
        return std::chrono::duration<double>(now - started_).count();
    }

    static double Rounded(double rate) {
        return std::round(rate * 10.0) / 10.0;
    }

    ambient::ipc::PipeServer& server_;
    ambient::translate::ITranslator* translator_ = nullptr;
    std::mutex throttle_mutex_;
    std::map<std::string, std::chrono::steady_clock::time_point> last_partial_;
    std::map<std::string, ambient::core::ThroughputMeter> meters_;
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
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
        // Flags first, then positional: pipe name, store root, models root, replay wav
        std::vector<std::string> args(argv + 1, argv + argc);
        const std::string asr_device = ambient::TakeFlag(args, "--asr-device");
        std::fprintf(stderr, "ambient-engine: power throttling %s\n",
                     ambient::host::Describe(ambient::host::DisableThrottlingOnSelf()).c_str());

        std::wstring pipe_name = L"\\\\.\\pipe\\LOCAL\\ambient-engine";
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
            store_root = std::filesystem::path(local_app_data) / "ambient" / "store";
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

        ambient::ipc::PipeServer server(pipe_name);
        WireEvents events(server);
        ambient::store::SqliteSessionStore session_store(store_root);
        // A consultation left by closing the app is left all the same
        session_store.EraseUnretained();
        ambient::models::ModelStore model_store(models_root);

        // A replay request plays a wav through the same port; a launch-time
        // wav path (CI, scripts) forces every session to replay that file
        ambient::audio::SourceFactory factory =
            [forced = args.size() > 3 ? args[3] : std::string()](
                const std::optional<ambient::audio::ReplaySpec>& replay,
                const std::string& mic_id) -> std::unique_ptr<ambient::audio::IAudioSource> {
            if (replay.has_value()) {
                return std::make_unique<ambient::audio::WavSource>(
                    replay->path, ambient::audio::WavSource::Config{replay->speed, replay->monitor,
                                                                    replay->start_frame});
            }
            if (!forced.empty()) {
                return std::make_unique<ambient::audio::WavSource>(forced);
            }
            return std::make_unique<ambient::audio::WasapiCapture>(ambient::audio::WideId(mic_id));
        };
        // Real transcription when the ASR role is staged, scripted otherwise (CI)
        ambient::models::OvRuntime ov_runtime;
        ambient::metrics::Registry metrics;
        bool first_use = false;
        std::unique_ptr<ambient::asr::ITranscriber> transcriber;
        try {
            model_store.Resolve("asr", "default");
            first_use =
                !std::filesystem::exists(model_store.Resolve("asr", "default").dir / ".cache");
            transcriber = std::make_unique<ambient::asr::WhisperTranscriber>(
                model_store, ov_runtime, asr_device, &metrics);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ambient-engine: scripted transcripts (%s)\n", e.what());
            transcriber = std::make_unique<ambient::asr::ScriptedTranscriber>();
        }
        // Same pattern for the VAD: real endpointing when staged. Compiles
        // behind the serve loop; session/start waits on it, hello does not
        std::unique_ptr<ambient::audio::IStreamingVad> vad;
        try {
            model_store.Resolve("vad", "default");
            vad = std::make_unique<ambient::audio::DeferredVad>([&model_store, &ov_runtime,
                                                                 &metrics] {
                const auto t0 = std::chrono::steady_clock::now();
                auto built = std::make_unique<ambient::audio::SileroVad>(model_store, ov_runtime);
                metrics.RecordLoad(
                    "vad",
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
                return built;
            });
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ambient-engine: capped windows (%s)\n", e.what());
            vad = std::make_unique<ambient::audio::PassthroughVad>();
        }
        // Diarisation needs both its models; without them turns simply keep
        // an empty speaker
        std::unique_ptr<ambient::diar::IDiariser> diariser;
        try {
            model_store.Resolve("diarisation", "default");
            model_store.Resolve("segmentation", "default");
            diariser = std::make_unique<ambient::diar::DeferredDiariser>([&model_store, &ov_runtime,
                                                                          store_root, &metrics] {
                const auto t0 = std::chrono::steady_clock::now();
                auto built = std::make_unique<ambient::diar::SpeakerDiariser>(
                    model_store, ov_runtime, store_root);
                metrics.RecordLoad(
                    "diarisation",
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
                return built;
            });
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ambient-engine: no speaker labels (%s)\n", e.what());
        }
        // Generation runs in its own supervised process: a GPU driver fault there
        // costs a respawn, never the engine (ADR-0027)
        std::unique_ptr<ambient::note::INoteWriter> note_writer;
        try {
            model_store.Resolve("note", "default");
            const auto prompt = models_root.parent_path() / "prompts";
            wchar_t exe_path[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
            const auto host =
                std::filesystem::path(exe_path).parent_path() / "ambient_note_host.exe";
            if (std::filesystem::exists(host)) {
                note_writer =
                    std::make_unique<ambient::note::WorkerNoteWriter>(host, models_root, prompt);
                // First use only: the one-off compile runs on an idle GPU, never inside a recording
                const auto note_dir = model_store.Resolve("note", "default").dir;
                if (!std::filesystem::exists(note_dir / ".cache")) {
                    first_use = true;
                    std::fprintf(stderr, "ambient-engine: first use, compiling the note model\n");
                    note_writer->Prepare();
                }
            } else {
                // Never write in-process: that is the configuration the driver fault corrupts
                std::fprintf(stderr, "ambient-engine: note DISABLED, %s is missing\n",
                             host.string().c_str());
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ambient-engine: stub note (%s)\n", e.what());
        }
        // Translation runs on the CPU, so it never contends with the GPU
        std::unique_ptr<ambient::translate::NllbTranslator> translator;
        std::unique_ptr<ambient::translate::TranslateLane> translate_lane;
        try {
            model_store.Resolve("translation", "default");
            translator =
                std::make_unique<ambient::translate::NllbTranslator>(model_store, ov_runtime);
            translate_lane = std::make_unique<ambient::translate::TranslateLane>(
                *translator,
                [&server, &events](const std::string& method, const nlohmann::json& params) {
                    if (method == "translate/partial") {
                        events.PushPartial(method, params);
                    } else {
                        if (method == "translate/ready") {
                            events.PushStreamEnd("translate/partial", "translate/ready", params);
                        } else if (method == "translate/failed") {
                            events.DropStream("translate/partial");
                            server.PushNotification(method, params);
                        } else {
                            server.PushNotification(method, params);
                        }
                    }
                });
            events.SetTranslator(translator.get());
            // First use: the CPU compile joins the one-off warm-up, so the
            // first translation is as fast as every other
            const auto translation_dir = model_store.Resolve("translation", "default").dir;
            if (!std::filesystem::exists(translation_dir / ".cache")) {
                first_use = true;
                std::fprintf(stderr, "ambient-engine: first use, compiling the translator\n");
                translator->Prepare();
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ambient-engine: no translation (%s)\n", e.what());
        }
        // 10 s, not 3: a Bluetooth microphone link waking measured 1.6-8.8 s
        // before first audio; wired mics answer in well under a second either way
        ambient::audio::SessionController controller(
            std::move(factory), events, session_store, *transcriber, *vad, std::chrono::seconds(10),
            diariser.get(), 5 * ambient::audio::kSampleRate, note_writer.get(), &metrics);

        ambient::ipc::RegisterMethods(server, controller, model_store, session_store, &metrics,
                                      &ov_runtime, translator.get(), translate_lane.get(),
                                      first_use);
        server.ServeOneClient();
        controller.Stop();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ambient-engine: %s\n", e.what());
        return 1;
    }
}
