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

// Every method the engine serves
void RegisterMethods(PipeServer& server, sotto::audio::SessionController& controller,
                     const sotto::models::ModelStore& models);

}  // namespace sotto::ipc
