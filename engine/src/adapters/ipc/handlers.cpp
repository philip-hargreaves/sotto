#include "adapters/ipc/handlers.hpp"

#include <cstdio>
#include <memory>
#include <openvino/core/version.hpp>
#include <optional>

#include "adapters/host/power_throttling.hpp"
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

json HandleAudioInputs(const std::vector<sotto::audio::CaptureDevice>& devices) {
    json list = json::array();
    for (const auto& device : devices) {
        list.push_back({{"id", device.id},
                        {"name", device.name},
                        {"shortName", device.short_name},
                        {"isDefault", device.is_default},
                        {"bluetooth", device.bluetooth}});
    }
    return json{{"devices", std::move(list)}};
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

// Optional stamps are empty strings in the store and null on the wire
json NullWhenEmpty(const std::string& value) {
    return value.empty() ? json(nullptr) : json(value);
}

}  // namespace

json HandleSessionList(sotto::store::ISessionStore& sessions) {
    json list = json::array();
    for (const auto& session : sessions.ListSessions()) {
        list.push_back({{"id", session.id},
                        {"startedAt", session.started_at},
                        {"endedAt", session.ended_at},
                        {"state", session.state},
                        {"sampleRate", session.sample_rate},
                        {"label", session.label},
                        {"editedAt", NullWhenEmpty(session.edited_at)},
                        {"audioSeconds", session.audio_seconds}});
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
        const auto note =
            sessions.ReadDocument(std::get<std::string>(id), sotto::store::DocumentKind::kNote);
        return json{{"text", note.text},
                    {"style", note.style},
                    {"detail", note.detail},
                    {"generatedAt", NullWhenEmpty(note.generated_at)},
                    {"editedAt", NullWhenEmpty(note.edited_at)}};
    } catch (const std::exception& e) {
        return Error{kSessionError, "Session error", json(e.what())};
    }
}

std::variant<json, Error> HandleSessionPatient(sotto::store::ISessionStore& sessions,
                                               const json& params) {
    const auto id = IdFrom(params);
    if (std::holds_alternative<Error>(id)) return std::get<Error>(id);
    try {
        using sotto::store::DocumentKind;
        const auto patient =
            sessions.ReadDocument(std::get<std::string>(id), DocumentKind::kPatient);
        const auto translation =
            sessions.ReadDocument(std::get<std::string>(id), DocumentKind::kTranslation);
        json result{{"text", patient.text},
                    {"language", patient.language},
                    {"generatedAt", NullWhenEmpty(patient.generated_at)},
                    {"editedAt", NullWhenEmpty(patient.edited_at)},
                    {"translation", nullptr}};
        if (!translation.text.empty()) {
            result["translation"] =
                json{{"language", translation.language}, {"text", translation.text}};
        }
        return result;
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
        return json{{"firstUse", first_use},
                    {"ready", ready("asr") && ready("note") && ready("translation")}};
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
                {"openvino", std::string(ov::get_openvino_version().buildNumber)},
                {"powerThrottling", host::Describe(host::ReadThrottling(GetCurrentProcess()))}};
        });
    }
    server.RegisterMethod("engine/models", [&models](const json&) { return HandleModels(models); });
    // Enumerated fresh per call, so a picker opened after a headset is
    // plugged in sees it without any notification plumbing
    server.RegisterMethod("audio/inputs", [](const json&) {
        return HandleAudioInputs(sotto::audio::ListCaptureDevices());
    });
    server.RegisterMethod("session/list",
                          [&sessions](const json&) { return HandleSessionList(sessions); });
    server.RegisterMethod("session/transcript", [&sessions](const json& params) {
        return HandleSessionTranscript(sessions, params);
    });
    server.RegisterMethod("session/note", [&sessions](const json& params) {
        return HandleSessionNote(sessions, params);
    });
    server.RegisterMethod("session/patient", [&sessions](const json& params) {
        return HandleSessionPatient(sessions, params);
    });
    // A typed label outlives regenerations; the note's own first sentence
    // fills in until then
    server.RegisterMethod(
        "session/label", [&sessions](const json& params) -> std::variant<json, Error> {
            const auto id = IdFrom(params);
            if (std::holds_alternative<Error>(id)) return std::get<Error>(id);
            if (!params.contains("text") || !params["text"].is_string()) {
                return Error{kInvalidParams, "Invalid params", json("text must be a string")};
            }
            try {
                sessions.EditDocument(std::get<std::string>(id), sotto::store::DocumentKind::kLabel,
                                      params["text"].get<std::string>());
                return json::object();
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
                    const auto text = sessions
                                          .ReadDocument(std::get<std::string>(id),
                                                        sotto::store::DocumentKind::kPatient)
                                          .text;
                    if (text.empty()) {
                        return Error{kSessionError, "Session error",
                                     json("no patient information to translate")};
                    }
                    // Stored before translate/ready goes out, so the sheet
                    // read back after it already carries the translation
                    const auto session_id = std::get<std::string>(id);
                    const auto on_ready = [&sessions, session_id](const std::string& translated,
                                                                  const std::string& language) {
                        try {
                            sotto::store::Document document;
                            document.text = translated;
                            document.language = language;
                            sessions.SaveDocument(
                                session_id, sotto::store::DocumentKind::kTranslation, document);
                        } catch (...) {  // NOLINT(bugprone-empty-catch)
                        }
                    };
                    if (!translate_lane->Run(text, params["language"].get<std::string>(),
                                             on_ready)) {
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
            // micId pins the picker's choice; one that has gone falls back
            // to the default, logged, and the snapshot records the fallback
            sotto::audio::MicSelection mic;
            if (!replay.has_value()) {
                const std::string requested = params.value("micId", "");
                const auto device =
                    sotto::audio::ResolveMicrophone(sotto::audio::ListCaptureDevices(), requested);
                if (!requested.empty() && device.id != requested) {
                    std::fprintf(stderr, "sotto-engine: chosen microphone gone, using %s\n",
                                 device.name.empty() ? "the default" : device.name.c_str());
                }
                mic = {device.id, device.name};
            }
            // resume: a crashed session's id; its stored audio replays ahead
            // of the live source into the new session. retain false: the
            // session is erased once the consultation is left
            if (!controller.Start(std::move(replay), params.value("resume", ""),
                                  params.value("retain", true), mic)) {
                return Error{kCaptureFailed, "Capture failed", json(controller.LastEnd().detail)};
            }
            return json{{"sessionId", controller.CurrentSession()}};
        });
    // Structure and length of the next note; invalid values are refused
    server.RegisterMethod(
        "note/options", [&controller](const json& params) -> std::variant<json, Error> {
            const std::string style = params.value("style", "prose");
            const std::string detail = params.value("detail", "standard");
            if (style != "prose" && style != "soap") {
                return Error{kInvalidParams, "Invalid params", json("unknown style: " + style)};
            }
            if (detail != "concise" && detail != "standard" && detail != "detailed") {
                return Error{kInvalidParams, "Invalid params", json("unknown detail: " + detail)};
            }
            controller.SetNoteOptions({style, detail});
            return json::object();
        });
    // Rewrite the finalised note with new options; results stream as usual
    server.RegisterMethod(
        "note/regenerate", [&controller](const json& params) -> std::variant<json, Error> {
            const std::string style = params.value("style", "prose");
            const std::string detail = params.value("detail", "standard");
            if (style != "prose" && style != "soap") {
                return Error{kInvalidParams, "Invalid params", json("unknown style: " + style)};
            }
            if (detail != "concise" && detail != "standard" && detail != "detailed") {
                return Error{kInvalidParams, "Invalid params", json("unknown detail: " + detail)};
            }
            if (!controller.RegenerateNote({style, detail})) {
                return Error{kSessionError, "Session error",
                             json("no finalised session, or a note is already being written")};
            }
            return json::object();
        });
    // The clinician''s edits become the record
    server.RegisterMethod(
        "note/update", [&sessions](const json& params) -> std::variant<json, Error> {
            const auto id = IdFrom(params);
            if (std::holds_alternative<Error>(id)) return std::get<Error>(id);
            if (!params.contains("text") || !params["text"].is_string()) {
                return Error{kInvalidParams, "Invalid params", json("text must be a string")};
            }
            try {
                sessions.EditDocument(std::get<std::string>(id), sotto::store::DocumentKind::kNote,
                                      params["text"].get<std::string>());
                return json::object();
            } catch (const std::exception& e) {
                return Error{kSessionError, "Session error", json(e.what())};
            }
        });
    server.RegisterMethod(
        "patient/update", [&sessions](const json& params) -> std::variant<json, Error> {
            const auto id = IdFrom(params);
            if (std::holds_alternative<Error>(id)) return std::get<Error>(id);
            if (!params.contains("text") || !params["text"].is_string()) {
                return Error{kInvalidParams, "Invalid params", json("text must be a string")};
            }
            try {
                sessions.EditDocument(std::get<std::string>(id),
                                      sotto::store::DocumentKind::kPatient,
                                      params["text"].get<std::string>());
                return json::object();
            } catch (const std::exception& e) {
                return Error{kSessionError, "Session error", json(e.what())};
            }
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
    // A past session under review: regenerate and translate act on it as
    // on a fresh seal; Record closes the review
    server.RegisterMethod(
        "session/open", [&controller](const json& params) -> std::variant<json, Error> {
            const auto id = IdFrom(params);
            if (std::holds_alternative<Error>(id)) return std::get<Error>(id);
            if (!controller.Open(std::get<std::string>(id))) {
                return Error{kSessionError, "Session error",
                             json("recording, a note is being written, or no such session")};
            }
            return json::object();
        });
    server.RegisterMethod("session/close", [&controller](const json&) {
        controller.Close();
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
