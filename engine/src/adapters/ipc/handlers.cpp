#include "adapters/ipc/handlers.hpp"

#include <memory>
#include <openvino/core/version.hpp>
#include <optional>

#include "adapters/models/ov_runtime.hpp"
#include "adapters/translate/translate_lane.hpp"
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

std::variant<json, Error> HandleSessionNote(sotto::store::ISessionStore& sessions,
                                            const json& params) {
    const auto id = IdFrom(params);
    if (std::holds_alternative<Error>(id)) return std::get<Error>(id);
    try {
        return json{{"text", sessions.ReadNote(std::get<std::string>(id))}};
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
                     const sotto::models::ModelStore& models, sotto::store::ISessionStore& sessions,
                     sotto::metrics::Registry* metrics, sotto::models::OvRuntime* runtime,
                     sotto::translate::ITranslator* translator,
                     sotto::translate::TranslateLane* translate_lane, bool first_use) {
    server.RegisterMethod("engine/hello", HandleHello);
    server.RegisterMethod("engine/echo", HandleEcho);
    // Ready when every staged model's compile cache exists: OpenVINO writes
    // the blob exactly when a compile completes, so no event plumbing needed
    server.RegisterMethod("engine/readiness", [&models, first_use](const json&) {
        const auto ready = [&models](const char* role) {
            try {
                const auto cache = models.Resolve(role, "default").dir / ".cache";
                return std::filesystem::exists(cache) && !std::filesystem::is_empty(cache);
            } catch (...) {
                return true;  // role not staged: nothing to wait for
            }
        };
        return json{{"firstUse", first_use}, {"ready", ready("asr") && ready("note")}};
    });
    if (metrics != nullptr) {
        // Device names are enumerated once, on the first fetch
        auto hardware = std::make_shared<std::optional<json>>();
        server.RegisterMethod("engine/metrics", [metrics, runtime, hardware](const json&) {
            if (!hardware->has_value()) {
                *hardware = runtime != nullptr ? json(runtime->DescribeDevices()) : json::object();
            }
            const auto s = metrics->Take();
            return json{
                {"devices", s.devices},
                {"loadSeconds", s.load_seconds},
                {"stageSeconds", s.stage_seconds},
                {"asrRealtimeFactor",
                 s.decode_busy_seconds > 0 ? s.decoded_audio_seconds / s.decode_busy_seconds : 0},
                {"audioSeconds", s.session_audio_seconds},
                {"lostFrames", s.lost_frames},
                {"diarTicks", s.diar_ticks},
                {"turns", s.turns},
                {"clusters", s.clusters},
                {"replay", s.replay},
                {"replaySpeed", s.replay_speed},
                {"hardware", **hardware},
                {"openvino", std::string(ov::get_openvino_version().buildNumber)}};
        });
    }
    server.RegisterMethod("engine/models", [&models](const json&) { return HandleModels(models); });
    server.RegisterMethod("session/list",
                          [&sessions](const json&) { return HandleSessionList(sessions); });
    server.RegisterMethod("session/transcript", [&sessions](const json& params) {
        return HandleSessionTranscript(sessions, params);
    });
    server.RegisterMethod("session/note", [&sessions](const json& params) {
        return HandleSessionNote(sessions, params);
    });
    server.RegisterMethod(
        "session/patient", [&sessions](const json& params) -> std::variant<json, Error> {
            const auto id = IdFrom(params);
            if (std::holds_alternative<Error>(id)) return std::get<Error>(id);
            try {
                return json{{"text", sessions.ReadPatient(std::get<std::string>(id))}};
            } catch (const std::exception& e) {
                return Error{kSessionError, "Session error", json(e.what())};
            }
        });
    if (translator != nullptr && translate_lane != nullptr) {
        server.RegisterMethod("translate/languages", [translator](const json&) {
            return json{{"languages", translator->Languages()}};
        });
        // Translates the session's patient sheet off the RPC thread; results
        // arrive as translate/partial then translate/ready
        server.RegisterMethod(
            "patient/translate",
            [&sessions, translate_lane](const json& params) -> std::variant<json, Error> {
                const auto id = IdFrom(params);
                if (std::holds_alternative<Error>(id)) return std::get<Error>(id);
                if (!params.contains("language") || !params["language"].is_string()) {
                    return Error{kInvalidParams, "Invalid params",
                                 json("language must be a string")};
                }
                try {
                    const auto text = sessions.ReadPatient(std::get<std::string>(id));
                    if (text.empty()) {
                        return Error{kSessionError, "Session error",
                                     json("no patient information to translate")};
                    }
                    if (!translate_lane->Run(text, params["language"].get<std::string>())) {
                        return Error{kSessionError, "Session error",
                                     json("a translation is already running")};
                    }
                    return json::object();
                } catch (const std::exception& e) {
                    return Error{kSessionError, "Session error", json(e.what())};
                }
            });
    }
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
            // resume: a crashed session's id; its stored audio replays ahead
            // of the live source into the new session
            if (!controller.Start(std::move(replay), params.value("resume", ""))) {
                return Error{kCaptureFailed, "Capture failed", json(controller.LastEnd().detail)};
            }
            return json{{"sessionId", controller.CurrentSession()}};
        });
    server.RegisterMethod("session/pause", [&controller](const json& params) {
        controller.SetPaused(params.value("paused", true));
        return json::object();
    });
    server.RegisterMethod("session/monitor", [&controller](const json& params) {
        controller.SetMonitor(params.value("on", true));
        return json::object();
    });
    server.RegisterMethod("session/cancel", [&controller](const json&) {
        controller.Cancel();
        return json::object();
    });
    // The note and patient lanes announce themselves when a writer is
    // wired; without one the stubs keep the contract for CI
    server.RegisterMethod("session/stop", [&server, &controller](const json&) {
        controller.Stop();
        if (!controller.HasNoteWriter()) {
            server.QueueNotification("note/ready", json::object());
            server.QueueNotification("patient/ready", json::object());
        }
        return json{{"sessionId", controller.LastFinalised()}};
    });
}

}  // namespace sotto::ipc
