#include "core/window_cutter.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace sotto::audio {
namespace {

std::vector<float> Ramp(std::size_t frames, float offset = 0.0f) {
    std::vector<float> audio(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        audio[i] = offset + static_cast<float>(i);
    }
    return audio;
}

TEST(WindowCutter, TheDefaultWindowIsBackgroundSized) {
    EXPECT_EQ(WindowCutter::kWindowFrames, 30u * 16000u);
}

TEST(WindowCutter, EmitsNothingUntilAWindowFills) {
    WindowCutter cutter(100);
    EXPECT_TRUE(cutter.Push(Ramp(99)).empty());
    const auto windows = cutter.Push(Ramp(1, 99.0f));
    ASSERT_EQ(windows.size(), 1u);
    EXPECT_EQ(windows[0].first_frame, 0u);
    EXPECT_EQ(windows[0].frames, Ramp(100));
}

TEST(WindowCutter, CutsExactWindowsAcrossUnevenPushes) {
    WindowCutter cutter(100);
    std::vector<WindowCutter::Window> windows;
    const auto audio = Ramp(250);
    for (std::size_t offset = 0; offset < audio.size(); offset += 37) {
        const auto count = std::min<std::size_t>(37, audio.size() - offset);
        for (auto& window : cutter.Push(std::span(audio).subspan(offset, count))) {
            windows.push_back(std::move(window));
        }
    }

    ASSERT_EQ(windows.size(), 2u);
    EXPECT_EQ(windows[0].first_frame, 0u);
    EXPECT_EQ(windows[1].first_frame, 100u);
    EXPECT_EQ(windows[1].frames.front(), 100.0f);
    EXPECT_EQ(windows[1].frames.back(), 199.0f);
}

TEST(WindowCutter, FlushReturnsTheTailExactlyOnce) {
    WindowCutter cutter(100);
    cutter.Push(Ramp(250));

    const auto tail = cutter.Flush();
    ASSERT_TRUE(tail.has_value());
    EXPECT_EQ(tail->first_frame, 200u);
    EXPECT_EQ(tail->frames.size(), 50u);
    EXPECT_EQ(tail->frames.front(), 200.0f);

    EXPECT_FALSE(cutter.Flush().has_value());
}

TEST(WindowCutter, FlushWithNothingPendingIsEmpty) {
    WindowCutter cutter(100);
    EXPECT_FALSE(cutter.Flush().has_value());
    cutter.Push(Ramp(100));
    EXPECT_FALSE(cutter.Flush().has_value()) << "an exact window leaves no tail";
}

TEST(WindowCutter, WindowsAndTailReassembleTheInput) {
    WindowCutter cutter(64);
    const auto audio = Ramp(1000);
    std::vector<float> joined;
    for (std::size_t offset = 0; offset < audio.size(); offset += 111) {
        const auto count = std::min<std::size_t>(111, audio.size() - offset);
        for (const auto& window : cutter.Push(std::span(audio).subspan(offset, count))) {
            joined.insert(joined.end(), window.frames.begin(), window.frames.end());
        }
    }
    if (const auto tail = cutter.Flush()) {
        joined.insert(joined.end(), tail->frames.begin(), tail->frames.end());
    }
    EXPECT_EQ(joined, audio);
}

}  // namespace
}  // namespace sotto::audio
