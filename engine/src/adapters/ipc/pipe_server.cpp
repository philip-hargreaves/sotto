#include "adapters/ipc/pipe_server.hpp"

#include <cstdio>
#include <system_error>
#include <utility>

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
        if (ov.hEvent == nullptr) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "CreateEventW");
        }
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

void PipeServer::QueueNotification(const std::string& method, json params) {
    notifications_.push_back(MakeNotification(method, std::move(params)));
}

void PipeServer::PushNotification(const std::string& method, json params) {
    constexpr unsigned kNotifyTimeoutMs = 2000;
    const std::string payload = Serialize(MakeNotification(method, std::move(params)));
    if (payload.size() > kMaxFrameBytes) {
        std::fputs("sotto-engine: pushed notification exceeded frame cap, dropped\n", stderr);
        return;
    }
    if (!WriteFrame(payload, kNotifyTimeoutMs)) {
        std::fputs("sotto-engine: notification push failed, client gone\n", stderr);
    }
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

    auto parsed = ParseRequest(std::move(message));
    if (std::holds_alternative<Error>(parsed)) {
        std::fputs("sotto-engine: dropped invalid request\n", stderr);
        return;
    }
    const auto& request = std::get<Request>(parsed);

    // A handler, or an oversized reply, must never escape into the serve loop
    try {
        const auto it = handlers_.find(request.method);
        if (it == handlers_.end()) {
            Reply(request.id, MakeError(request.id, Error{kMethodNotFound, "Method not found"}));
            return;
        }
        auto outcome = it->second(request.params);
        if (std::holds_alternative<Error>(outcome)) {
            Reply(request.id, MakeError(request.id, std::get<Error>(outcome)));
        } else {
            Reply(request.id, MakeResult(request.id, std::get<json>(outcome)));
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sotto-engine: handler failed: %s\n", e.what());
        Reply(request.id, MakeError(request.id, Error{kInternalError, "Internal error"}));
    }
    FlushNotifications();
}

void PipeServer::FlushNotifications() {
    for (const auto& notification : notifications_) {
        const std::string payload = Serialize(notification);
        if (payload.size() > kMaxFrameBytes) {
            std::fputs("sotto-engine: notification exceeded frame cap, dropped\n", stderr);
            continue;
        }
        if (!WriteFrame(payload)) {
            std::fputs("sotto-engine: notification write failed, client gone\n", stderr);
            break;
        }
    }
    notifications_.clear();
}

void PipeServer::Reply(const Id& id, const json& envelope) {
    std::string payload = Serialize(envelope);
    if (payload.size() > kMaxFrameBytes) {
        std::fputs("sotto-engine: reply exceeded frame cap, sent internal error\n", stderr);
        payload = Serialize(MakeError(id, Error{kInternalError, "Internal error"}));
    }
    if (!WriteFrame(payload)) {
        std::fputs("sotto-engine: reply write failed, client gone\n", stderr);
    }
}

bool PipeServer::WriteFrame(const std::string& payload, unsigned timeout_ms) {
    const std::lock_guard<std::mutex> lock(write_mutex_);
    if (write_failed_) return false;
    const std::string frame = EncodeFrame(payload);
    HANDLE pipe = static_cast<HANDLE>(pipe_);
    std::size_t written_total = 0;
    while (written_total < frame.size()) {
        OverlappedEvent write;
        DWORD written = 0;
        if (!WriteFile(pipe, frame.data() + written_total,
                       static_cast<DWORD>(frame.size() - written_total), nullptr, &write.ov)) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            // A client that stops draining must not stall the writer; a
            // cancel tears the frame, so the stream is declared dead
            if (timeout_ms != 0 &&
                WaitForSingleObject(write.ov.hEvent, timeout_ms) != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &write.ov);
                CompleteOverlapped(pipe, write.ov, written);
                write_failed_ = true;
                return false;
            }
        }
        if (!CompleteOverlapped(pipe, write.ov, written)) return false;
        written_total += written;
    }
    return true;
}

}  // namespace sotto::ipc
