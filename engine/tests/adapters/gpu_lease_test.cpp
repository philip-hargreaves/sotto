#include "adapters/host/gpu_lease.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace ambient::host {
namespace {

TEST(GpuLease, UnnamedLeaseIsInertAndFree) {
    GpuLease lease("");
    EXPECT_FALSE(lease.Active());
    const auto guard = lease.Acquire();
    EXPECT_EQ(guard.waited(), 0.0);
}

TEST(GpuLease, TwoHoldersOfOneNameTakeTurns) {
    const std::string name = "Local\\ambient-gpu-lease-test-" + std::to_string(GetCurrentProcessId());
    GpuLease engine(name);
    GpuLease host(name);  // a second handle, as the other process would open it
    ASSERT_TRUE(engine.Active());
    ASSERT_TRUE(host.Active());

    std::atomic<bool> inside{false};
    std::atomic<bool> overlapped{false};
    auto first = engine.Acquire();
    std::thread other([&] {
        const auto guard = host.Acquire();
        overlapped = inside.load();
        EXPECT_GT(guard.waited(), 0.05);
    });
    inside = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    inside = false;
    first = GpuLease::Guard{};  // release
    other.join();
    EXPECT_FALSE(overlapped) << "the second holder must wait for the first to release";
}

}  // namespace
}  // namespace ambient::host
