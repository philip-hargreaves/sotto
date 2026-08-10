#include "adapters/ipc/pipe_security.hpp"

#include <stdexcept>
#include <system_error>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <aclapi.h>
#include <windows.h>

namespace sotto::ipc {

namespace {

[[noreturn]] void ThrowLastError(const char* what) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), what);
}

std::vector<char> QueryTokenGroups(HANDLE token) {
    DWORD size = 0;
    GetTokenInformation(token, TokenGroups, nullptr, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) ThrowLastError("GetTokenInformation size");
    std::vector<char> buffer(size);
    if (!GetTokenInformation(token, TokenGroups, buffer.data(), size, &size)) {
        ThrowLastError("GetTokenInformation");
    }
    return buffer;
}

}  // namespace

struct PipeSecurity::Impl {
    std::vector<char> groups;
    PSID system_sid = nullptr;
    PACL dacl = nullptr;
    SECURITY_DESCRIPTOR descriptor{};
    SECURITY_ATTRIBUTES attributes{};

    ~Impl() {
        if (dacl != nullptr) LocalFree(dacl);
        if (system_sid != nullptr) FreeSid(system_sid);
    }
};

PipeSecurity::PipeSecurity() : impl_(std::make_unique<Impl>()) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        ThrowLastError("OpenProcessToken");
    }
    impl_->groups = QueryTokenGroups(token);
    CloseHandle(token);

    // The logon SID scopes access to this login session; another user on the
    // same machine, or the same user in another session, gets nothing
    const auto* groups = reinterpret_cast<TOKEN_GROUPS*>(impl_->groups.data());
    PSID logon_sid = nullptr;
    for (DWORD i = 0; i < groups->GroupCount; ++i) {
        if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) != 0) {
            logon_sid = groups->Groups[i].Sid;
            break;
        }
    }
    if (logon_sid == nullptr) throw std::runtime_error("no logon SID in process token");

    SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(&nt_authority, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0,
                                  &impl_->system_sid)) {
        ThrowLastError("AllocateAndInitializeSid");
    }

    // Everything a duplex client needs, and never FILE_APPEND_DATA: that bit
    // doubles as FILE_CREATE_PIPE_INSTANCE, the right to spawn rogue instances
    const DWORD client_rights = FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES |
                                FILE_WRITE_ATTRIBUTES | FILE_READ_EA | FILE_WRITE_EA |
                                READ_CONTROL | SYNCHRONIZE;

    EXPLICIT_ACCESS_W entries[2] = {};
    for (auto& e : entries) {
        e.grfAccessPermissions = client_rights;
        e.grfAccessMode = SET_ACCESS;
        e.grfInheritance = NO_INHERITANCE;
        e.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    }
    entries[0].Trustee.ptstrName = static_cast<LPWSTR>(logon_sid);
    entries[1].Trustee.ptstrName = static_cast<LPWSTR>(impl_->system_sid);

    if (SetEntriesInAclW(2, entries, nullptr, &impl_->dacl) != ERROR_SUCCESS) {
        throw std::runtime_error("SetEntriesInAclW failed");
    }
    if (!InitializeSecurityDescriptor(&impl_->descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&impl_->descriptor, TRUE, impl_->dacl, FALSE)) {
        ThrowLastError("security descriptor setup");
    }
    impl_->attributes = {sizeof(SECURITY_ATTRIBUTES), &impl_->descriptor, FALSE};
}

PipeSecurity::~PipeSecurity() = default;

void* PipeSecurity::Attributes() const {
    return &impl_->attributes;
}

}  // namespace sotto::ipc
