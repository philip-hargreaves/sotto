// The note model's own process: a driver fault here costs a respawn, never
// the engine. Speaks JSON-RPC on a private pipe; exits when the engine goes.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _DEBUG
#include <crtdbg.h>
#endif

#include "adapters/host/power_throttling.hpp"
#include "adapters/ipc/pipe_server.hpp"
#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "adapters/note/qwen_note_writer.hpp"
#include "ports/transcriber.hpp"

namespace {

// One generation at a time, off the RPC thread so partials stream while
// the pipe stays responsive to cancel
class GenerationLane {
   public:
    explicit GenerationLane(ambient::ipc::PipeServer& server) : server_(server) {}

    ~GenerationLane() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    bool Run(std::function<std::string(const ambient::note::INoteWriter::Progress&)> generate) {
        if (running_.exchange(true)) {
            return false;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        thread_ = std::thread([this, generate = std::move(generate)] {
            try {
                const auto t0 = std::chrono::steady_clock::now();
                bool first = true;
                const std::string text = generate([this, &t0, &first](const std::string& partial) {
                    if (first) {
                        first = false;
                        std::fprintf(
                            stderr, "ambient-note-host: first token in %.1f s\n",
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
                                .count());
                    }
                    server_.PushNotification("partial", {{"text", partial}});
                });
                server_.PushNotification("ready", {{"text", text}});
            } catch (const std::exception& e) {
                server_.PushNotification("failed", {{"detail", e.what()}});
            } catch (...) {
                server_.PushNotification("failed", {{"detail", "note generation failed"}});
            }
            running_ = false;
        });
        return true;
    }

   private:
    ambient::ipc::PipeServer& server_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

std::vector<ambient::asr::Turn> TurnsFrom(const nlohmann::json& params) {
    std::vector<ambient::asr::Turn> turns;
    for (const auto& t : params.value("turns", nlohmann::json::array())) {
        turns.push_back({t.value("firstFrame", std::uint64_t{0}),
                         t.value("frameCount", std::uint64_t{0}), t.value("speaker", ""),
                         t.value("text", "")});
    }
    return turns;
}

}  // namespace

int main(int argc, char* argv[]) {
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    try {
        if (argc < 4) {
            std::fprintf(stderr, "usage: ambient_note_host <pipe> <models> <prompts-dir>\n");
            return 2;
        }
        std::fprintf(stderr, "ambient-note-host: power throttling %s\n",
                     ambient::host::Describe(ambient::host::DisableThrottlingOnSelf()).c_str());
        const std::wstring pipe_name = std::filesystem::path(argv[1]).wstring();
        const std::filesystem::path models_root = argv[2];
        const std::filesystem::path prompt_path = argv[3];

        ambient::ipc::PipeServer server(pipe_name);
        ambient::models::ModelStore store(models_root);
        ambient::models::OvRuntime runtime;
        ambient::note::QwenNoteWriter writer(store, runtime, prompt_path);
        GenerationLane lane(server);

        using ambient::ipc::Error;
        using ambient::ipc::json;
        using ambient::ipc::kSessionError;
        server.RegisterMethod("prepare", [&writer](const json&) {
            writer.Prepare();
            return json::object();
        });
        server.RegisterMethod("cancel", [&writer](const json&) {
            writer.Cancel();
            return json::object();
        });
        // Inline: short, and a failure is only a lost guess, never an error
        // the engine's next attempt could mistake for its own
        server.RegisterMethod("prefill", [&writer](const json& params) {
            try {
                writer.Prefill(TurnsFrom(params), {params.value("style", "prose"), "standard"});
            } catch (const std::exception& e) {
                std::fprintf(stderr, "ambient-note-host: prefill dropped (%s)\n", e.what());
            }
            return json::object();
        });
        server.RegisterMethod(
            "write", [&writer, &lane](const json& params) -> std::variant<json, Error> {
                auto turns = TurnsFrom(params);
                ambient::note::NoteOptions options{params.value("style", "prose"),
                                                   params.value("detail", "standard")};
                options.confirmed = params.value("confirmed", false);
                if (!lane.Run([&writer, turns = std::move(turns),
                               options = std::move(options)](const auto& progress) {
                        return writer.Write(turns, options, progress);
                    })) {
                    return Error{kSessionError, "Session error", json("a generation is running")};
                }
                return json::object();
            });
        server.RegisterMethod(
            "label", [&writer, &lane](const json& params) -> std::variant<json, Error> {
                std::string note = params.value("note", "");
                if (!lane.Run([&writer, note = std::move(note)](const auto&) {
                        return writer.WriteLabel(note);
                    })) {
                    return Error{kSessionError, "Session error", json("a generation is running")};
                }
                return json::object();
            });
        server.RegisterMethod(
            "writePatient", [&writer, &lane](const json& params) -> std::variant<json, Error> {
                std::string note = params.value("note", "");
                if (!lane.Run([&writer, note = std::move(note)](const auto& progress) {
                        return writer.WritePatient(note, progress);
                    })) {
                    return Error{kSessionError, "Session error", json("a generation is running")};
                }
                return json::object();
            });

        // The engine is the one client; its death ends this serve loop and
        // the process with it, so a worker can never outlive its engine
        server.ServeOneClient();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ambient-note-host: fatal: %s\n", e.what());
        return 1;
    }
}
