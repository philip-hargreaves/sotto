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

json HandleModels(const sotto::models::ModelStore& models) {
    json list = json::array();
    for (const auto& model : models.List()) {
        list.push_back({{"id", model.id},
                        {"task", model.task},
                        {"tier", model.tier},
                        {"device", model.device},
                        {"licence", model.licence}});
    }
    return json{{"models", std::move(list)}};
}

void RegisterMethods(PipeServer& server, sotto::audio::SessionController& controller,
                     const sotto::models::ModelStore& models) {
    server.RegisterMethod("engine/hello", HandleHello);
    server.RegisterMethod("engine/echo", HandleEcho);
    server.RegisterMethod("engine/models", [&models](const json&) { return HandleModels(models); });
    server.RegisterMethod("session/start", [&controller](const json&) -> std::variant<json, Error> {
        if (!controller.Start()) {
            return Error{kCaptureFailed, "Capture failed", json(controller.LastEnd().detail)};
        }
        return json::object();
    });
    server.RegisterMethod("session/cancel", [&controller](const json&) {
        controller.Cancel();
        return json::object();
    });
    // Stub pipeline:
    server.RegisterMethod("session/stop", [&server, &controller](const json&) {
        controller.Stop();
        server.QueueNotification("note/ready", json::object());
        server.QueueNotification("patient/ready", json::object());
        return json::object();
    });
}

}  // namespace sotto::ipc
