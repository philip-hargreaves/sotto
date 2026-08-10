#include "adapters/ipc/pipe_server.hpp"

#include <cstdio>
#include <system_error>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "adapters/ipc/framing.hpp"

namespace sotto::ipc {

namespace {

constexpr DWORD kIoBufferBytes = 64 * 1024;

// Waits out a pending overlapped operation; false means the pipe is gone
bool CompleteOverlapped(HANDLE pipe, OVERLAPPED& ov, DWORD& transferred) {
    if (GetOverlappedResult(pipe, &ov, &transferred, TRUE)) return true;
    return false;
}

struct OverlappedEvent {
    OVERLAPPED ov{};
    OverlappedEvent() {
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ~OverlappedEvent() {
        if (ov.hEvent != nullptr) CloseHandle(ov.hEvent);
    }
};

}  // namespace

PipeServer::PipeServer(const std::wstring& pipe_name) {
    HANDLE pipe = CreateNamedPipeW(
        pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
        kIoBufferBytes, kIoBufferBytes, 0,
        static_cast<SECURITY_ATTRIBUTES*>(security_.Attributes()));
    if (pipe == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_ACCESS_DENIED) {
            throw std::runtime_error("pipe name already claimed by another process");
        }
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "CreateNamedPipeW");
    }
    pipe_ = pipe;
}

PipeServer::~PipeServer() {
    if (pipe_ != nullptr) CloseHandle(pipe_);
}

void PipeServer::RegisterMethod(const std::string& method, MethodHandler handler) {
    handlers_[method] = std::move(handler);
}

void PipeServer::ServeOneClient() {
    HANDLE pipe = static_cast<HANDLE>(pipe_);

    OverlappedEvent connect;
    if (!ConnectNamedPipe(pipe, &connect.ov)) {
        const DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            DWORD ignored = 0;
            if (!CompleteOverlapped(pipe, connect.ov, ignored)) return;
        } else if (error != ERROR_PIPE_CONNECTED) {
            return;
        }
    }

    FrameDecoder decoder;
    char buffer[kIoBufferBytes];
    for (;;) {
        OverlappedEvent read;
        DWORD transferred = 0;
        if (!ReadFile(pipe, buffer, sizeof(buffer), nullptr, &read.ov)) {
            if (GetLastError() != ERROR_IO_PENDING) break;
        }
        if (!CompleteOverlapped(pipe, read.ov, transferred)) break;
        if (transferred == 0) break;

        decoder.Push(std::string_view(buffer, transferred));
        std::optional<std::string> payload;
        while ((payload = decoder.Next())) {
            HandleFrame(*payload);
        }
        if (decoder.failed()) break;
    }
    DisconnectNamedPipe(pipe);
}

void PipeServer::HandleFrame(const std::string& payload) {
    json message;
    try {
        message = json::parse(payload);
    } catch (const json::parse_error&) {
        // No trustworthy id to answer with, so log and drop
        std::fputs("sotto-engine: dropped unparseable frame\n", stderr);
        return;
    }

    auto parsed = ParseRequest(message);
    if (std::holds_alternative<Error>(parsed)) {
        std::fputs("sotto-engine: dropped invalid request\n", stderr);
        return;
    }
    const auto& request = std::get<Request>(parsed);

    const auto it = handlers_.find(request.method);
    if (it == handlers_.end()) {
        WriteFrame(Serialize(MakeError(
            request.id, Error{kMethodNotFound, "Method not found", json(request.method)})));
        return;
    }

    auto outcome = it->second(request.params);
    if (std::holds_alternative<Error>(outcome)) {
        WriteFrame(Serialize(MakeError(request.id, std::get<Error>(outcome))));
    } else {
        WriteFrame(Serialize(MakeResult(request.id, std::get<json>(outcome))));
    }
}

void PipeServer::WriteFrame(const std::string& payload) {
    const std::string frame = EncodeFrame(payload);
    HANDLE pipe = static_cast<HANDLE>(pipe_);
    std::size_t written_total = 0;
    while (written_total < frame.size()) {
        OverlappedEvent write;
        DWORD written = 0;
        if (!WriteFile(pipe, frame.data() + written_total,
                       static_cast<DWORD>(frame.size() - written_total), nullptr, &write.ov)) {
            if (GetLastError() != ERROR_IO_PENDING) return;
        }
        if (!CompleteOverlapped(pipe, write.ov, written)) return;
        written_total += written;
    }
}

}  // namespace sotto::ipc
