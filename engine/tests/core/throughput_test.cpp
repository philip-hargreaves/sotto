#include "core/throughput.hpp"

#include <gtest/gtest.h>

namespace sotto::core {
namespace {

TEST(Throughput, ASteadyStreamReadsItsRate) {
    ThroughputMeter meter;
    for (int i = 0; i <= 20; ++i) {
        meter.Token(i * 0.05);  // 20 tokens per second
    }
    EXPECT_NEAR(meter.Rate(1.0), 20.0, 0.5);
    EXPECT_NEAR(meter.Average(), 20.0, 0.5);
}

TEST(Throughput, TheWindowRollsAndTheAverageDoesNot) {
    ThroughputMeter meter(2.0);
    for (int i = 0; i < 20; ++i) {
        meter.Token(i * 0.1);  // 10 tok/s for two seconds
    }
    for (int i = 0; i <= 50; ++i) {
        meter.Token(2.0 + i * 0.02);  // then 50 tok/s for one second
    }
    EXPECT_GT(meter.Rate(3.1), 25.0) << "the live rate follows the stream";
    // 70 tokens over 3 s: the average is the whole generation's truth
    EXPECT_NEAR(meter.Average(), 70.0 / 3.0, 0.5);
}

TEST(Throughput, AStallDecaysTheRateButNotTheAverage) {
    ThroughputMeter meter(2.0);
    meter.Token(0.0);
    meter.Token(0.1);
    meter.Token(0.2);
    EXPECT_EQ(meter.Rate(5.0), 0);
    EXPECT_NEAR(meter.Average(), 10.0, 0.1);
}

TEST(Throughput, DegenerateInputsAreQuiet) {
    ThroughputMeter meter;
    EXPECT_EQ(meter.Rate(1.0), 0);
    EXPECT_EQ(meter.Average(), 0);
    meter.Token(1.0);
    EXPECT_EQ(meter.Rate(1.5), 0) << "one token is not a rate";
    EXPECT_EQ(meter.Average(), 0);
    meter.Reset();
    meter.Token(2.0);
    meter.Token(2.0);  // same stamp: no span
    EXPECT_EQ(meter.Average(), 0);
}

}  // namespace
}  // namespace sotto::core
