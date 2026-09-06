#pragma once

#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace ambient::host {

// Keeps the system out of standby for the object's lifetime: an idle timeout
// mid-load or mid-generation leaves GPU work the driver may never finish.
// Best effort
class AwakeRequest {
   public:
    explicit AwakeRequest(const wchar_t* reason) {
        REASON_CONTEXT context{};
        context.Version = POWER_REQUEST_CONTEXT_VERSION;
        context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        context.Reason.SimpleReasonString = const_cast<wchar_t*>(reason);
        request_ = PowerCreateRequest(&context);
        if (request_ != INVALID_HANDLE_VALUE && request_ != nullptr) {
            PowerSetRequest(request_, PowerRequestSystemRequired);
        }
    }

    ~AwakeRequest() {
        if (request_ != INVALID_HANDLE_VALUE && request_ != nullptr) {
            PowerClearRequest(request_, PowerRequestSystemRequired);
            CloseHandle(request_);
        }
    }

    AwakeRequest(const AwakeRequest&) = delete;
    AwakeRequest& operator=(const AwakeRequest&) = delete;

   private:
    HANDLE request_ = INVALID_HANDLE_VALUE;
};

}  // namespace ambient::host
