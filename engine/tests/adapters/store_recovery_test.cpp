#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "adapters/storage/chunk_cipher.hpp"
#include "adapters/storage/db.hpp"
#include "adapters/storage/sqlite_session_store.hpp"

namespace sotto::store {
namespace {

float PatternAt(std::uint64_t frame) {
    return static_cast<float>(frame % 1000) / 1000.0f - 0.5f;
}

struct TempRoot {
    std::filesystem::path path;

    TempRoot() {
        path = std::filesystem::temp_directory_path() /
               ("sotto-crash-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    }

    ~TempRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

// The victim with its stdout piped back; killed hard, never asked to exit
class HelperProcess {
   public:
    HelperProcess(const std::filesystem::path& root, const char* mode) {
        SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE write = nullptr;
        if (!CreatePipe(&read_, &write, &inheritable, 0) ||
            !SetHandleInformation(read_, HANDLE_FLAG_INHERIT, 0)) {
            throw std::runtime_error("pipe setup failed");
        }

        std::string command = std::string(SOTTO_CRASH_HELPER) + " \"" + root.string() + "\"";
        if (mode != nullptr) {
            command += std::string(" ") + mode;
        }

        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = write;
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION process{};
        const BOOL launched = CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, 0,
                                             nullptr, nullptr, &startup, &process);
        CloseHandle(write);
        if (!launched) {
            throw std::runtime_error("could not launch " + command);
        }
        process_ = process.hProcess;
        CloseHandle(process.hThread);
    }

    ~HelperProcess() {
        Kill();
        CloseHandle(read_);
        CloseHandle(process_);
    }

    void Kill() {
        if (!killed_) {
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, INFINITE);
            killed_ = true;
        }
    }

    // Blocks until a line arrives or the pipe closes (empty on EOF)
    std::string ReadLine() {
        for (;;) {
            const auto newline = buffer_.find('\n');
            if (newline != std::string::npos) {
                std::string line = buffer_.substr(0, newline);
                buffer_.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                return line;
            }
            char chunk[256];
            DWORD got = 0;
            if (!ReadFile(read_, chunk, sizeof(chunk), &got, nullptr) || got == 0) {
                return {};
            }
            buffer_.append(chunk, got);
        }
    }

   private:
    HANDLE process_ = nullptr;
    HANDLE read_ = nullptr;
    std::string buffer_;
    bool killed_ = false;
};

TEST(StoreRecovery, AHardKilledSessionRecoversEveryAckedChunk) {
    TempRoot root;
    std::string session_id;
    std::int64_t acked = 0;
    {
        HelperProcess helper(root.path, nullptr);
        for (;;) {
            const std::string line = helper.ReadLine();
            ASSERT_FALSE(line.empty()) << "helper died before enough chunks were committed";
            if (line.starts_with("SESSION ")) {
                session_id = line.substr(8);
            } else if (line.starts_with("CHUNKS ")) {
                acked = std::stoll(line.substr(7));
                if (acked >= 5) {
                    break;
                }
            }
        }
        helper.Kill();
    }
    ASSERT_FALSE(session_id.empty());

    // The catalog finds the crashed session
    SqliteSessionStore reopened(root.path, std::chrono::hours(1));
    const auto recoverable = reopened.ScanRecoverable();
    ASSERT_EQ(recoverable.size(), 1u);
    EXPECT_EQ(recoverable[0].id, session_id);

    // Every acked chunk survived the kill, decrypts, and carries the exact
    // frames that were appended
    Db db(root.path / "sotto.db");
    Db::Stmt key = db.Prepare("SELECT wrapped FROM session_keys WHERE session_id = ?");
    key.BindText(1, session_id);
    ASSERT_TRUE(key.Step()) << "the key row was committed with the session";
    const ChunkCipher cipher = ChunkCipher::FromWrapped(key.ColumnBlob(0));

    Db::Stmt select = db.Prepare(
        "SELECT seq, first_frame, frame_count, payload FROM chunks WHERE session_id = ?"
        " ORDER BY seq");
    select.BindText(1, session_id);
    std::int64_t expected_seq = 0;
    std::uint64_t expected_frame = 0;
    while (select.Step()) {
        ASSERT_EQ(select.ColumnInt64(0), expected_seq) << "a committed chunk is missing";
        ASSERT_EQ(select.ColumnInt64(1), static_cast<std::int64_t>(expected_frame));
        const std::vector<std::uint8_t> plain =
            cipher.Open(Domain::kAudio, session_id, static_cast<std::uint64_t>(expected_seq),
                        select.ColumnBlob(3));
        ASSERT_EQ(plain.size(), static_cast<std::size_t>(select.ColumnInt64(2)) * sizeof(float));
        std::vector<float> frames(plain.size() / sizeof(float));
        std::memcpy(frames.data(), plain.data(), plain.size());
        for (const float sample : frames) {
            ASSERT_EQ(sample, PatternAt(expected_frame)) << "frame " << expected_frame;
            ++expected_frame;
        }
        ++expected_seq;
    }
    EXPECT_GE(expected_seq, acked) << "an acked commit was lost";

    // Turns commit synchronously, so every one before the kill survives too
    Db::Stmt turns = db.Prepare("SELECT seq, payload FROM turns WHERE session_id = ? ORDER BY seq");
    turns.BindText(1, session_id);
    std::int64_t turn_seq = 0;
    while (turns.Step()) {
        ASSERT_EQ(turns.ColumnInt64(0), turn_seq);
        const auto plain = cipher.Open(Domain::kTurns, session_id,
                                       static_cast<std::uint64_t>(turn_seq), turns.ColumnBlob(1));
        const std::string content(plain.begin(), plain.end());
        EXPECT_NE(content.find("turn " + std::to_string(turn_seq)), std::string::npos);
        ++turn_seq;
    }
    EXPECT_GT(turn_seq, 0) << "the killed session wrote no turns";
}

TEST(StoreRecovery, AHardKillAfterCancelLeavesNothing) {
    TempRoot root;
    {
        HelperProcess helper(root.path, "cancel");
        for (;;) {
            const std::string line = helper.ReadLine();
            ASSERT_FALSE(line.empty()) << "helper died before cancelling";
            if (line == "CANCELLED") {
                break;
            }
        }
        helper.Kill();
    }

    SqliteSessionStore reopened(root.path, std::chrono::hours(1));
    EXPECT_TRUE(reopened.ScanRecoverable().empty());
    EXPECT_TRUE(reopened.ListSessions().empty());
    Db db(root.path / "sotto.db");
    EXPECT_EQ(db.QueryInt64("SELECT COUNT(*) FROM session_keys"), 0) << "the key went first";
    EXPECT_EQ(db.QueryInt64("SELECT COUNT(*) FROM chunks"), 0);
}

}  // namespace
}  // namespace sotto::store
