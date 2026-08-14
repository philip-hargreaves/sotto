#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>

#include "adapters/audio/wasapi_capture.hpp"
#include "adapters/audio/wav_source.hpp"
#include "adapters/ipc/handlers.hpp"
#include "adapters/ipc/pipe_server.hpp"
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
        // Tests pass a private pipe name so runs cannot collide with the app
        std::wstring pipe_name = L"\\\\.\\pipe\\LOCAL\\sotto-engine";
        if (argc > 1) {
            pipe_name = L"\\\\.\\pipe\\" + std::wstring(argv[1], argv[1] + std::strlen(argv[1]));
        }

        sotto::ipc::PipeServer server(pipe_name);
        WireEvents events(server);

        // A wav path replays through the same port instead of the microphone
        sotto::audio::SourceFactory factory;
        if (argc > 2) {
            factory = [path = std::string(argv[2])] {
                return std::make_unique<sotto::audio::WavSource>(path);
            };
        } else {
            factory = [] { return std::make_unique<sotto::audio::WasapiCapture>(); };
        }
        sotto::audio::SessionController controller(std::move(factory), events);

        sotto::ipc::RegisterMethods(server, controller);
        server.ServeOneClient();
        controller.Stop();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sotto-engine: %s\n", e.what());
        return 1;
    }
}
