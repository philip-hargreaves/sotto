#pragma once

#include <variant>

#include "adapters/audio/capture_devices.hpp"
#include "adapters/diarisation/anchor_store.hpp"
#include "adapters/ipc/messages.hpp"
#include "adapters/ipc/pipe_server.hpp"
#include "adapters/models/model_store.hpp"
#include "core/session_controller.hpp"

namespace ambient::models {
class OvRuntime;
}  // namespace ambient::models

namespace ambient::translate {
class ITranslator;
class TranslateLane;
}  // namespace ambient::translate

namespace ambient::ipc {

std::variant<json, Error> HandleHello(const json& params);

std::variant<json, Error> HandleEcho(const json& params);

json HandleModels(const ambient::models::ModelStore& models);

json HandleAudioInputs(const std::vector<ambient::audio::CaptureDevice>& devices);

// The clinician's voiceprint: where it came from and how many consultations
// refined it. Clearing is refused while a session runs
json HandleAnchorStatus(const ambient::diar::AnchorStore& anchors);
std::variant<json, Error> HandleAnchorClear(ambient::diar::AnchorStore& anchors,
                                            bool session_active);

json HandleSessionList(ambient::store::ISessionStore& sessions);

std::variant<json, Error> HandleSessionNote(ambient::store::ISessionStore& sessions,
                                            const json& params);

std::variant<json, Error> HandleSessionPatient(ambient::store::ISessionStore& sessions,
                                               const json& params);

std::variant<json, Error> HandleSessionTranscript(ambient::store::ISessionStore& sessions,
                                                  const json& params);

std::variant<json, Error> HandleSessionDelete(ambient::store::ISessionStore& sessions,
                                              const json& params);

// Every method the engine serves. first_use: model caches were cold at
// launch, so the one-off compiles are running and readiness reports them.
void RegisterMethods(PipeServer& server, ambient::audio::SessionController& controller,
                     const ambient::models::ModelStore& models,
                     ambient::store::ISessionStore& sessions,
                     ambient::metrics::Registry* metrics = nullptr,
                     ambient::models::OvRuntime* runtime = nullptr,
                     ambient::translate::ITranslator* translator = nullptr,
                     ambient::translate::TranslateLane* translate_lane = nullptr,
                     bool first_use = false, ambient::diar::AnchorStore* anchors = nullptr);

}  // namespace ambient::ipc
