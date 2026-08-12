#include "adapters/ipc/pipe_security.hpp"

#include <gtest/gtest.h>

#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace sotto::ipc {
namespace {

// The rights the pipe is meant to grant, spelled out independently of the
// production code so a change there has to be made deliberately here too.
constexpr DWORD kExpectedRights = FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES |
                                  FILE_WRITE_ATTRIBUTES | FILE_READ_EA | FILE_WRITE_EA |
                                  READ_CONTROL | SYNCHRONIZE;

struct ParsedAce {
    BYTE type = 0;
    DWORD mask = 0;
    PSID sid = nullptr;
};

SECURITY_ATTRIBUTES* AttributesOf(const PipeSecurity& security) {
    return static_cast<SECURITY_ATTRIBUTES*>(security.Attributes());
}

PACL DaclOf(const PipeSecurity& security, BOOL& present, BOOL& defaulted) {
    PACL dacl = nullptr;
    auto* descriptor =
        static_cast<PSECURITY_DESCRIPTOR>(AttributesOf(security)->lpSecurityDescriptor);
    EXPECT_TRUE(GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted));
    return dacl;
}

std::vector<ParsedAce> AcesOf(const PipeSecurity& security) {
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = DaclOf(security, present, defaulted);
    std::vector<ParsedAce> aces;
    if (dacl == nullptr) return aces;

    ACL_SIZE_INFORMATION info{};
    if (!GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation)) return aces;
    for (DWORD i = 0; i < info.AceCount; ++i) {
        void* raw = nullptr;
        if (!GetAce(dacl, i, &raw)) continue;
        const auto* header = static_cast<ACE_HEADER*>(raw);
        ParsedAce parsed;
        parsed.type = header->AceType;
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(raw);
            parsed.mask = allowed->Mask;
            parsed.sid = static_cast<PSID>(&allowed->SidStart);
        }
        aces.push_back(parsed);
    }
    return aces;
}

// The logon SID of this process, derived here rather than taken from the
// production code, so the test can disagree with it.
std::vector<char> ProcessTokenGroups() {
    HANDLE token = nullptr;
    EXPECT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    DWORD size = 0;
    GetTokenInformation(token, TokenGroups, nullptr, 0, &size);
    std::vector<char> buffer(size);
    EXPECT_TRUE(GetTokenInformation(token, TokenGroups, buffer.data(), size, &size));
    CloseHandle(token);
    return buffer;
}

PSID LogonSidWithin(const std::vector<char>& groups_buffer) {
    const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(groups_buffer.data());
    for (DWORD i = 0; i < groups->GroupCount; ++i) {
        if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) != 0) {
            return groups->Groups[i].Sid;
        }
    }
    return nullptr;
}

struct SystemSid {
    PSID sid = nullptr;
    SystemSid() {
        SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
        EXPECT_TRUE(
            AllocateAndInitializeSid(&nt, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &sid));
    }
    ~SystemSid() {
        if (sid != nullptr) FreeSid(sid);
    }
    SystemSid(const SystemSid&) = delete;
    SystemSid& operator=(const SystemSid&) = delete;
};

TEST(PipeSecurity, AttributesAreWellFormedAndNotInheritable) {
    PipeSecurity security;
    auto* attributes = AttributesOf(security);

    ASSERT_NE(attributes, nullptr);
    EXPECT_EQ(attributes->nLength, sizeof(SECURITY_ATTRIBUTES));
    EXPECT_NE(attributes->lpSecurityDescriptor, nullptr);
    // An inheritable handle would hand the pipe to any child process
    EXPECT_FALSE(attributes->bInheritHandle);
}

TEST(PipeSecurity, DaclIsPresentAndNotDefaulted) {
    PipeSecurity security;
    BOOL present = FALSE;
    BOOL defaulted = FALSE;

    PACL dacl = DaclOf(security, present, defaulted);

    // A missing DACL is not an empty one: it grants everyone everything
    EXPECT_TRUE(present);
    EXPECT_NE(dacl, nullptr);
    EXPECT_FALSE(defaulted);
}

TEST(PipeSecurity, GrantsExactlyTwoAllowAces) {
    PipeSecurity security;

    const auto aces = AcesOf(security);

    ASSERT_EQ(aces.size(), 2u);
    for (const auto& ace : aces) {
        EXPECT_EQ(ace.type, ACCESS_ALLOWED_ACE_TYPE);
    }
}

TEST(PipeSecurity, WithholdsCreatePipeInstance) {
    PipeSecurity security;

    const auto aces = AcesOf(security);

    // FILE_APPEND_DATA and FILE_CREATE_PIPE_INSTANCE are the same bit, so
    // granting the convenient combined right would let a client squat the name
    ASSERT_FALSE(aces.empty());
    for (const auto& ace : aces) {
        EXPECT_EQ(ace.mask & FILE_APPEND_DATA, 0u);
        EXPECT_EQ(ace.mask & FILE_CREATE_PIPE_INSTANCE, 0u);
        EXPECT_NE(ace.mask & FILE_GENERIC_WRITE, FILE_GENERIC_WRITE);
    }
}

TEST(PipeSecurity, GrantsOnlyTheExpectedRights) {
    PipeSecurity security;

    const auto aces = AcesOf(security);

    ASSERT_FALSE(aces.empty());
    for (const auto& ace : aces) {
        EXPECT_EQ(ace.mask, kExpectedRights);
    }
}

TEST(PipeSecurity, TrusteesAreThisLogonSessionAndSystem) {
    PipeSecurity security;
    const auto groups = ProcessTokenGroups();
    PSID logon_sid = LogonSidWithin(groups);
    ASSERT_NE(logon_sid, nullptr);
    const SystemSid system;

    const auto aces = AcesOf(security);

    ASSERT_EQ(aces.size(), 2u);
    bool has_logon = false;
    bool has_system = false;
    for (const auto& ace : aces) {
        ASSERT_NE(ace.sid, nullptr);
        ASSERT_TRUE(IsValidSid(ace.sid));
        if (EqualSid(ace.sid, logon_sid)) has_logon = true;
        if (EqualSid(ace.sid, system.sid)) has_system = true;
    }
    // The logon SID scopes the pipe to this login, not merely to this user
    EXPECT_TRUE(has_logon);
    EXPECT_TRUE(has_system);
}

}  // namespace
}  // namespace sotto::ipc
