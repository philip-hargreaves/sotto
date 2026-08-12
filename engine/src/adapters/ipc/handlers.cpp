#include "adapters/ipc/handlers.hpp"

#include "core/version.hpp"

namespace sotto::ipc {

std::variant<json, Error> HandleHello(const json& params) {
    const auto peer = PeerInfoFromJson(params);
    if (!peer) {
        return Error{kInvalidParams, "Invalid params",
                     json("expected name, version, protocolVersion")};
    }
    return ToJson(PeerInfo{sotto::kName, sotto::kVersion, kProtocolVersion});
}

std::variant<json, Error> HandleEcho(const json& params) {
    if (!params.contains("payload") || !params["payload"].is_string()) {
        return Error{kInvalidParams, "Invalid params", json("expected payload string")};
    }
    return json{{"payload", params["payload"]}};
}

void RegisterMethods(PipeServer& server) {
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
}

}  // namespace sotto::ipc
