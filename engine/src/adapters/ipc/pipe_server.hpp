#pragma once

#include <functional>
#include <map>
#include <string>
#include <variant>

#include "adapters/ipc/messages.hpp"
#include "adapters/ipc/pipe_security.hpp"

namespace sotto::ipc {

// One duplex pipe, one client at a time. Construction claims the pipe
// name so a name already taken is treated as an attack and throws, never retries.
class PipeServer {
   public:
    using MethodHandler = std::function<std::variant<json, Error>(const json& params)>;

    explicit PipeServer(const std::wstring& pipe_name);
    ~PipeServer();
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    void RegisterMethod(const std::string& method, MethodHandler handler);

    // Blocks: accept one client, serve until it disconnects or the stream corrupts
    void ServeOneClient();

   private:
    void HandleFrame(const std::string& payload);
    void Reply(const Id& id, const json& envelope);
    bool WriteFrame(const std::string& payload);

    PipeSecurity security_;
    void* pipe_ = nullptr;  // HANDLE
    std::map<std::string, MethodHandler> handlers_;
};

}  // namespace sotto::ipc
