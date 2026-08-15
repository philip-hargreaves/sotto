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
#include "adapters/ipc/handlers.hpp"
#include "adapters/ipc/pipe_server.hpp"
#include "adapters/models/model_store.hpp"
#include "adapters/storage/sqlite_session_store.hpp"
#include "core/session_controller.hpp"

namespace {

class WireEvents : public sotto::audio::ISessionEvents {
   public:
    explicit WireEvents(sotto::ipc::PipeServer& server) : server_(server) {}

    void OnLevel(const sotto::audio::LevelReading& reading) override {
        server_.PushNotification("audio.level",
                                 {{"level", reading.level}, {"clipped", reading.clipped}});
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
            models_root = std::filesystem::path(exe_path).parent_path() / "models";
        }

        sotto::ipc::PipeServer server(pipe_name);
        WireEvents events(server);
        sotto::store::SqliteSessionStore session_store(store_root);
        sotto::models::ModelStore model_store(models_root);

        // A wav path replays through the same port instead of the microphone
        sotto::audio::SourceFactory factory;
        if (argc > 4) {
            factory = [path = std::string(argv[4])] {
                return std::make_unique<sotto::audio::WavSource>(path);
            };
        } else {
            factory = [] { return std::make_unique<sotto::audio::WasapiCapture>(); };
        }
        sotto::audio::SessionController controller(std::move(factory), events, session_store);

        sotto::ipc::RegisterMethods(server, controller, model_store);
        server.ServeOneClient();
        controller.Stop();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sotto-engine: %s\n", e.what());
        return 1;
    }
}
