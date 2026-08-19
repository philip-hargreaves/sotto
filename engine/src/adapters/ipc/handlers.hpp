#pragma once

#include <variant>

#include "adapters/ipc/messages.hpp"
#include "adapters/ipc/pipe_server.hpp"
#include "adapters/models/model_store.hpp"
#include "core/session_controller.hpp"

namespace sotto::ipc {

std::variant<json, Error> HandleHello(const json& params);

std::variant<json, Error> HandleEcho(const json& params);

json HandleModels(const sotto::models::ModelStore& models);

json HandleSessionList(sotto::store::ISessionStore& sessions);

std::variant<json, Error> HandleSessionNote(sotto::store::ISessionStore& sessions,
                                            const json& params);

std::variant<json, Error> HandleSessionTranscript(sotto::store::ISessionStore& sessions,
                                                  const json& params);

std::variant<json, Error> HandleSessionDelete(sotto::store::ISessionStore& sessions,
                                              const json& params);

// Every method the engine serves
void RegisterMethods(PipeServer& server, sotto::audio::SessionController& controller,
                     const sotto::models::ModelStore& models,
                     sotto::store::ISessionStore& sessions);

}  // namespace sotto::ipc
