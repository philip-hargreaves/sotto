#pragma once

#include <memory>

namespace ambient::ipc {

// DACL granting pipe access to this logon session and SYSTEM only, built from
// individual rights so a client can never create its own pipe instance
class PipeSecurity {
   public:
    PipeSecurity();
    ~PipeSecurity();
    PipeSecurity(const PipeSecurity&) = delete;
    PipeSecurity& operator=(const PipeSecurity&) = delete;

    // SECURITY_ATTRIBUTES*, kept as void* so windows.h stays out of the header
    void* Attributes() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ambient::ipc
