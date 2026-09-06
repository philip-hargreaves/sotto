// A note host that loads nothing: speaks the host protocol so the lane's
// state machine can be exercised with no weights and no GPU. A tier whose
// model id contains "broken" fails to load; everything else loads in 100 ms
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#include "adapters/ipc/pipe_server.hpp"
#include "adapters/models/model_store.hpp"

int main(int argc, char* argv[]) {
    try {
        if (argc < 4) {
            std::fprintf(stderr, "usage: fake_note_host <pipe> <models> <prompts-dir> [tier]\n");
            return 2;
        }
        const std::wstring pipe_name = std::filesystem::path(argv[1]).wstring();
        const std::string tier = argc > 4 ? argv[4] : "default";
        ambient::models::ModelStore store(argv[2]);
        const ambient::models::ModelInfo& info = store.Resolve("note", tier);

        ambient::ipc::PipeServer server(pipe_name);
        using ambient::ipc::json;
        std::thread loader;
        server.RegisterMethod("prepare", [&](const json&) {
            if (loader.joinable()) loader.join();
            loader = std::thread([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (info.id.find("broken") != std::string::npos) {
                    server.PushNotification(
                        "loadFailed",
                        {{"id", info.id}, {"name", info.name}, {"detail", "no such device"}});
                } else {
                    server.PushNotification("loaded", {{"id", info.id},
                                                       {"name", info.name},
                                                       {"seconds", 0.1},
                                                       {"firstUse", false}});
                }
            });
            return json::object();
        });
        server.RegisterMethod("cancel", [](const json&) { return json::object(); });
        server.RegisterMethod("prefill", [](const json&) { return json::object(); });
        server.RegisterMethod("write", [&](const json&) {
            server.PushNotification("partial", {{"text", "A note from " + info.id}});
            server.PushNotification("ready", {{"text", "A note from " + info.id}});
            return json::object();
        });
        server.ServeOneClient();
        if (loader.joinable()) loader.join();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fake-note-host: fatal: %s\n", e.what());
        return 1;
    }
}
