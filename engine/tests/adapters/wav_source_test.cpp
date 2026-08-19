#include "adapters/audio/wav_source.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace sotto::audio {
namespace {

// Every file is built byte by byte in the test, so each malformation is
// deliberate rather than an accident of a fixture on disk.
struct WavSpec {
    std::uint16_t format = 1;  // 1 = PCM, 3 = float
    std::uint16_t channels = 1;
    std::uint32_t sample_rate = 16000;
    std::uint16_t bits_per_sample = 16;
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> chunk_before_data;
    std::optional<std::uint32_t> declared_data_bytes;
};

template <typename T>
void AppendValue(std::vector<std::uint8_t>& out, T value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

void AppendTag(std::vector<std::uint8_t>& out, const char* tag) {
    out.insert(out.end(), tag, tag + 4);
}

std::vector<std::uint8_t> Build(const WavSpec& spec) {
    std::vector<std::uint8_t> body;
    AppendTag(body, "WAVE");

    AppendTag(body, "fmt ");
    AppendValue<std::uint32_t>(body, 16);
    AppendValue(body, spec.format);
    AppendValue(body, spec.channels);
    AppendValue(body, spec.sample_rate);
    const std::uint32_t frame_bytes = spec.channels * spec.bits_per_sample / 8u;
    AppendValue<std::uint32_t>(body, spec.sample_rate * frame_bytes);
    AppendValue<std::uint16_t>(body, static_cast<std::uint16_t>(frame_bytes));
    AppendValue(body, spec.bits_per_sample);

    body.insert(body.end(), spec.chunk_before_data.begin(), spec.chunk_before_data.end());

    AppendTag(body, "data");
    AppendValue<std::uint32_t>(
        body, spec.declared_data_bytes.value_or(static_cast<std::uint32_t>(spec.data.size())));
    body.insert(body.end(), spec.data.begin(), spec.data.end());

    std::vector<std::uint8_t> file;
    AppendTag(file, "RIFF");
    AppendValue<std::uint32_t>(file, static_cast<std::uint32_t>(body.size()));
    file.insert(file.end(), body.begin(), body.end());
    return file;
}

struct TempWav {
    std::filesystem::path path;

    explicit TempWav(const std::vector<std::uint8_t>& bytes) {
        path = std::filesystem::temp_directory_path() /
               ("sotto-wav-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".wav");
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    ~TempWav() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
};

struct RecordingSink : IAudioSink {
    std::vector<float> frames;
    std::vector<std::size_t> packet_sizes;
    std::uint64_t lost = 0;
    std::vector<SourceEnd> ends;

    void OnAudio(std::span<const float> packet, std::uint64_t lost_frames) override {
        frames.insert(frames.end(), packet.begin(), packet.end());
        packet_sizes.push_back(packet.size());
        lost += lost_frames;
    }

    void OnEnd(const SourceEnd& end) override {
        ends.push_back(end);
    }
};

std::vector<std::uint8_t> Pcm16Bytes(const std::vector<std::int16_t>& samples) {
    std::vector<std::uint8_t> bytes;
    for (const auto sample : samples) {
        AppendValue(bytes, sample);
    }
    return bytes;
}

std::vector<std::uint8_t> FloatBytes(const std::vector<float>& samples) {
    std::vector<std::uint8_t> bytes;
    for (const auto sample : samples) {
        AppendValue(bytes, sample);
    }
    return bytes;
}

TEST(WavSource, Pcm16RoundTripsWithPinnedScaling) {
    const TempWav file(Build({.data = Pcm16Bytes({-32768, -16384, 0, 16384, 32767})}));
    WavSource source(file.path.string());
    RecordingSink sink;

    source.Run(sink);

    ASSERT_EQ(sink.ends.size(), 1u);
    EXPECT_EQ(sink.ends[0].reason, SourceEndReason::kCompleted);
    ASSERT_EQ(sink.frames.size(), 5u);
    EXPECT_EQ(sink.frames[0], -1.0F);
    EXPECT_EQ(sink.frames[1], -0.5F);
    EXPECT_EQ(sink.frames[2], 0.0F);
    EXPECT_EQ(sink.frames[3], 0.5F);
    EXPECT_NEAR(sink.frames[4], 1.0F, 0.0001F);
    EXPECT_EQ(sink.lost, 0u);
}

TEST(WavSource, Float32RoundTripsExactly) {
    const std::vector<float> samples{0.25F, -0.75F, 1.0F, -1.0F};
    const TempWav file(Build({.format = 3, .bits_per_sample = 32, .data = FloatBytes(samples)}));
    WavSource source(file.path.string());
    RecordingSink sink;

    source.Run(sink);

    ASSERT_EQ(sink.ends.size(), 1u);
    EXPECT_EQ(sink.ends[0].reason, SourceEndReason::kCompleted);
    EXPECT_EQ(sink.frames, samples);
}

TEST(WavSource, EmitsFixedPacketsWithAShortTail) {
    const TempWav file(Build({.data = Pcm16Bytes(std::vector<std::int16_t>(1000, 7))}));
    WavSource source(file.path.string());
    RecordingSink sink;

    source.Run(sink);

    EXPECT_EQ(sink.packet_sizes, (std::vector<std::size_t>{480, 480, 40}));
    EXPECT_EQ(sink.frames.size(), 1000u);
}

TEST(WavSource, SkipsUnknownChunksAndTheirPadByte) {
    std::vector<std::uint8_t> list_chunk;
    AppendTag(list_chunk, "LIST");
    AppendValue<std::uint32_t>(list_chunk, 3);  // Odd size forces the pad byte
    list_chunk.insert(list_chunk.end(), {'a', 'b', 'c', 0});
    const TempWav file(Build({.data = Pcm16Bytes({1, 2, 3}), .chunk_before_data = list_chunk}));
    WavSource source(file.path.string());
    RecordingSink sink;

    source.Run(sink);

    ASSERT_EQ(sink.ends.size(), 1u);
    EXPECT_EQ(sink.ends[0].reason, SourceEndReason::kCompleted);
    EXPECT_EQ(sink.frames.size(), 3u);
}

TEST(WavSource, ATruncatedDataChunkFailsNeverCompletes) {
    const TempWav file(Build(
        {.data = Pcm16Bytes(std::vector<std::int16_t>(500, 7)), .declared_data_bytes = 2000}));
    WavSource source(file.path.string());
    RecordingSink sink;

    source.Run(sink);

    ASSERT_EQ(sink.ends.size(), 1u);
    EXPECT_EQ(sink.ends[0].reason, SourceEndReason::kFailed);
    EXPECT_NE(sink.ends[0].detail.find("truncated"), std::string::npos);
    // The complete packet before the truncation point was still delivered
    EXPECT_EQ(sink.frames.size(), 480u);
}

TEST(WavSource, GarbageFailsAsNotARiffFile) {
    const TempWav file(std::vector<std::uint8_t>{'n', 'o', 't', ' ', 'a', ' ', 'w', 'a', 'v'});
    WavSource source(file.path.string());
    RecordingSink sink;

    source.Run(sink);

    ASSERT_EQ(sink.ends.size(), 1u);
    EXPECT_EQ(sink.ends[0].reason, SourceEndReason::kFailed);
    EXPECT_EQ(sink.ends[0].detail, "not a RIFF file");
    EXPECT_TRUE(sink.frames.empty());
}

TEST(WavSource, RefusesFormatsThePipelineDoesNotSpeak) {
    const struct {
        WavSpec spec;
        const char* expected;
    } cases[] = {
        {{.channels = 2, .data = Pcm16Bytes({1, 2})}, "mono"},
        {{.sample_rate = 44100, .data = Pcm16Bytes({1})}, "16000"},
        {{.bits_per_sample = 24, .data = {0, 0, 0}}, "PCM16 and float32"},
    };

    for (const auto& [spec, expected] : cases) {
        const TempWav file(Build(spec));
        WavSource source(file.path.string());
        RecordingSink sink;

        source.Run(sink);

        ASSERT_EQ(sink.ends.size(), 1u) << expected;
        EXPECT_EQ(sink.ends[0].reason, SourceEndReason::kFailed) << expected;
        EXPECT_NE(sink.ends[0].detail.find(expected), std::string::npos) << sink.ends[0].detail;
        EXPECT_TRUE(sink.frames.empty()) << expected;
    }
}

TEST(WavSource, DataThatIsNotWholeFramesFails) {
    const TempWav file(Build({.data = {1, 2, 3}}));  // 3 bytes of PCM16
    WavSource source(file.path.string());
    RecordingSink sink;

    source.Run(sink);

    ASSERT_EQ(sink.ends.size(), 1u);
    EXPECT_EQ(sink.ends[0].reason, SourceEndReason::kFailed);
    EXPECT_NE(sink.ends[0].detail.find("whole frames"), std::string::npos);
}

TEST(WavSource, StopMidStreamEndsAsStoppedNotCompleted) {
    struct StoppingSink : RecordingSink {
        WavSource* source = nullptr;

        void OnAudio(std::span<const float> packet, std::uint64_t lost_frames) override {
            RecordingSink::OnAudio(packet, lost_frames);
            source->RequestStop();
        }
    };

    const TempWav file(Build({.data = Pcm16Bytes(std::vector<std::int16_t>(2000, 7))}));
    WavSource source(file.path.string());
    StoppingSink sink;
    sink.source = &source;

    source.Run(sink);

    ASSERT_EQ(sink.ends.size(), 1u);
    EXPECT_EQ(sink.ends[0].reason, SourceEndReason::kStopped);
    EXPECT_EQ(sink.frames.size(), 480u);  // One packet, then the stop honoured
}

TEST(WavSource, RealTimeReplayIsPacedFlatOutIsNot) {
    // Half a second of audio: paced delivery takes roughly that long
    const TempWav file(Build({.data = Pcm16Bytes(std::vector<std::int16_t>(8000, 0))}));
    RecordingSink sink;

    const auto flat_start = std::chrono::steady_clock::now();
    WavSource(file.path.string()).Run(sink);
    const auto flat = std::chrono::steady_clock::now() - flat_start;
    EXPECT_LT(flat, std::chrono::milliseconds(200));

    const auto paced_start = std::chrono::steady_clock::now();
    WavSource(file.path.string(), {.speed = 1.0}).Run(sink);
    const auto paced = std::chrono::steady_clock::now() - paced_start;
    EXPECT_GT(paced, std::chrono::milliseconds(400));
    EXPECT_LT(paced, std::chrono::milliseconds(1500));
}

TEST(WavSource, PauseHoldsFramesAndResumeDeliversThemAll) {
    const TempWav file(Build({.data = Pcm16Bytes(std::vector<std::int16_t>(4800, 0))}));
    WavSource source(file.path.string());
    RecordingSink sink;
    source.SetPaused(true);

    std::thread runner([&] { source.Run(sink); });
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const auto held = sink.frames.size();
    source.SetPaused(false);
    runner.join();

    EXPECT_LE(held, 480u) << "at most the packet in flight before the pause was seen";
    EXPECT_EQ(sink.frames.size(), 4800u) << "paused audio is held, never dropped";
    ASSERT_EQ(sink.ends.size(), 1u);
    EXPECT_EQ(sink.ends[0].reason, SourceEndReason::kCompleted);
}

TEST(WavSource, StopWinsOverPause) {
    const TempWav file(Build({.data = Pcm16Bytes(std::vector<std::int16_t>(4800, 0))}));
    WavSource source(file.path.string());
    RecordingSink sink;
    source.SetPaused(true);

    std::thread runner([&] { source.Run(sink); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    source.RequestStop();
    runner.join();

    ASSERT_EQ(sink.ends.size(), 1u);
    EXPECT_EQ(sink.ends[0].reason, SourceEndReason::kStopped)
        << "a paused session can always be finalised";
}

}  // namespace
}  // namespace sotto::audio
