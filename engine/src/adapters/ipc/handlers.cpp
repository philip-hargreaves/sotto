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

namespace {

std::variant<std::string, Error> IdFrom(const json& params) {
    if (!params.contains("id") || !params["id"].is_string()) {
        return Error{kInvalidParams, "Invalid params", json("id must be a string")};
    }
    return params["id"].get<std::string>();
}

}  // namespace

json HandleSessionList(sotto::store::ISessionStore& sessions) {
    json list = json::array();
    for (const auto& session : sessions.ListSessions()) {
        list.push_back({{"id", session.id},
                        {"startedAt", session.started_at},
                        {"endedAt", session.ended_at},
                        {"state", session.state},
                        {"sampleRate", session.sample_rate}});
    }
    return json{{"sessions", std::move(list)}};
}

std::variant<json, Error> HandleSessionTranscript(sotto::store::ISessionStore& sessions,
                                                  const json& params) {
    const auto id = IdFrom(params);
    if (std::holds_alternative<Error>(id)) return std::get<Error>(id);
    try {
        json turns = json::array();
        for (const auto& turn : sessions.ReadTurns(std::get<std::string>(id))) {
            turns.push_back({{"firstFrame", turn.first_frame},
                             {"frameCount", turn.frame_count},
                             {"speaker", turn.speaker},
                             {"text", turn.text}});
        }
        return json{{"turns", std::move(turns)}};
    } catch (const std::exception& e) {
        return Error{kSessionError, "Session error", json(e.what())};
    }
}

std::variant<json, Error> HandleSessionDelete(sotto::store::ISessionStore& sessions,
                                              const json& params) {
    const auto id = IdFrom(params);
    if (std::holds_alternative<Error>(id)) return std::get<Error>(id);
    try {
        sessions.Delete(std::get<std::string>(id));
        return json::object();
    } catch (const std::exception& e) {
        return Error{kSessionError, "Session error", json(e.what())};
    }
}

void RegisterMethods(PipeServer& server, sotto::audio::SessionController& controller,
                     const sotto::models::ModelStore& models,
                     sotto::store::ISessionStore& sessions) {
    server.RegisterMethod("engine/hello", HandleHello);
    server.RegisterMethod("engine/echo", HandleEcho);
    server.RegisterMethod("engine/models", [&models](const json&) { return HandleModels(models); });
    server.RegisterMethod("session/list",
                          [&sessions](const json&) { return HandleSessionList(sessions); });
    server.RegisterMethod("session/transcript", [&sessions](const json& params) {
        return HandleSessionTranscript(sessions, params);
    });
    server.RegisterMethod("session/delete", [&sessions](const json& params) {
        return HandleSessionDelete(sessions, params);
    });
    server.RegisterMethod(
        "session/start", [&controller](const json& params) -> std::variant<json, Error> {
            // An optional replay block plays a file through the same
            // pipeline; absent means microphone
            std::optional<sotto::audio::ReplaySpec> replay;
            if (params.contains("replay")) {
                const auto& r = params["replay"];
                if (!r.contains("path") || !r["path"].is_string()) {
                    return Error{kInvalidParams, "replay.path is required", {}};
                }
                replay = sotto::audio::ReplaySpec{r["path"].get<std::string>(),
                                                  r.value("speed", 1.0), r.value("monitor", false)};
            }
            if (!controller.Start(std::move(replay))) {
                return Error{kCaptureFailed, "Capture failed", json(controller.LastEnd().detail)};
            }
            return json::object();
        });
    server.RegisterMethod("session/pause", [&controller](const json& params) {
        controller.SetPaused(params.value("paused", true));
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
        return json{{"sessionId", controller.LastFinalised()}};
    });
}

}  // namespace sotto::ipc
