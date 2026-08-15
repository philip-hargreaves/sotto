#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

#include "adapters/audio/wasapi_capture.hpp"
#include "adapters/audio/wav_source.hpp"
#include "adapters/ipc/handlers.hpp"
#include "adapters/ipc/pipe_server.hpp"
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
        // argv: [1] private pipe name, [2] store root, [3] replay wav path;
        // tests pass all three so runs collide with neither the app nor its store
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

        sotto::ipc::PipeServer server(pipe_name);
        WireEvents events(server);
        sotto::store::SqliteSessionStore session_store(store_root);

        // A wav path replays through the same port instead of the microphone
        sotto::audio::SourceFactory factory;
        if (argc > 3) {
            factory = [path = std::string(argv[3])] {
                return std::make_unique<sotto::audio::WavSource>(path);
            };
        } else {
            factory = [] { return std::make_unique<sotto::audio::WasapiCapture>(); };
        }
        sotto::audio::SessionController controller(std::move(factory), events, session_store);

        sotto::ipc::RegisterMethods(server, controller);
        server.ServeOneClient();
        controller.Stop();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sotto-engine: %s\n", e.what());
        return 1;
    }
}
