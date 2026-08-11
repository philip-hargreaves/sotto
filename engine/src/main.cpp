#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

#include "adapters/ipc/messages.hpp"
#include "adapters/ipc/pipe_server.hpp"
#include "core/version.hpp"

namespace {

using sotto::ipc::Error;
using sotto::ipc::json;

std::variant<json, Error> HandleHello(const json& params) {
    const auto peer = sotto::ipc::PeerInfoFromJson(params);
    if (!peer) {
        return Error{sotto::ipc::kInvalidParams, "Invalid params",
                     json("expected name, version, protocolVersion")};
    }
    return sotto::ipc::ToJson(
        sotto::ipc::PeerInfo{sotto::kName, sotto::kVersion, sotto::ipc::kProtocolVersion});
}

std::variant<json, Error> HandleEcho(const json& params) {
    if (!params.contains("payload") || !params["payload"].is_string()) {
        return Error{sotto::ipc::kInvalidParams, "Invalid params", json("expected payload string")};
    }
    return json{{"payload", params["payload"]}};
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        // Tests pass a private pipe name so runs cannot collide with the app
        std::wstring pipe_name = L"\\\\.\\pipe\\LOCAL\\sotto-engine";
        if (argc > 1) {
            pipe_name = L"\\\\.\\pipe\\" + std::wstring(argv[1], argv[1] + std::strlen(argv[1]));
        }

        sotto::ipc::PipeServer server(pipe_name);
        server.RegisterMethod("engine/hello", HandleHello);
        server.RegisterMethod("engine/echo", HandleEcho);
        server.RegisterMethod("session/start", [](const json&) { return json::object(); });
        server.RegisterMethod("session/cancel", [](const json&) { return json::object(); });
        // Stub pipeline: the real one will write the note before these fire
        server.RegisterMethod("session/stop", [&server](const json&) {
            server.QueueNotification("note/ready", json::object());
            server.QueueNotification("patient/ready", json::object());
            return json::object();
        });
        server.ServeOneClient();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sotto-engine: %s\n", e.what());
        return 1;
    }
}
