#pragma once

#include <variant>

#include "adapters/ipc/messages.hpp"
#include "adapters/ipc/pipe_server.hpp"
#include "core/session_controller.hpp"

namespace sotto::ipc {

std::variant<json, Error> HandleHello(const json& params);

std::variant<json, Error> HandleEcho(const json& params);

// Every method the engine serves, in one place
void RegisterMethods(PipeServer& server, sotto::audio::SessionController& controller);

}  // namespace sotto::ipc
