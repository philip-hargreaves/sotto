#include <cstdio>
#include <exception>

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

int main() {
    try {
        sotto::ipc::PipeServer server(L"\\\\.\\pipe\\LOCAL\\sotto-engine");
        server.RegisterMethod("engine/hello", HandleHello);
        server.RegisterMethod("engine/echo", HandleEcho);
        server.ServeOneClient();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sotto-engine: %s\n", e.what());
        return 1;
    }
}
