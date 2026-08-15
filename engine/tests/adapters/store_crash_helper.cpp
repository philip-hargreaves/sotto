// Kill victim for the recovery test: records through the real store until
// terminated. "cancel" mode cancels first, then idles to be killed.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

#include "adapters/storage/db.hpp"
#include "adapters/storage/sqlite_session_store.hpp"

namespace {

float PatternAt(std::uint64_t frame) {
    return static_cast<float>(frame % 1000) / 1000.0f - 0.5f;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        return 2;
    }
    const std::filesystem::path root = argv[1];
    const bool cancel_mode = argc > 2 && std::strcmp(argv[2], "cancel") == 0;

    sotto::store::SqliteSessionStore store(root, std::chrono::milliseconds(20));
    const auto id = store.Begin({16000, "", ""});
    std::printf("SESSION %s\n", id.c_str());
    std::fflush(stdout);

    std::vector<float> audio(1600);
    std::uint64_t frame = 0;
    if (cancel_mode) {
        for (int i = 0; i < 10; ++i) {
            for (auto& sample : audio) sample = PatternAt(frame++);
            store.Append(id, audio, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        store.Cancel(id);
        std::printf("CANCELLED\n");
        std::fflush(stdout);
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // A second connection watches the committed count; the acked number is
    // what the test holds recovery to
    sotto::store::Db reader(root / "sessions" / (id + ".db"));
    for (;;) {
        for (auto& sample : audio) sample = PatternAt(frame++);
        store.Append(id, audio, 0);
        try {
            std::printf("CHUNKS %lld\n",
                        static_cast<long long>(reader.QueryInt64("SELECT COUNT(*) FROM chunks")));
            std::fflush(stdout);
        } catch (...) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
