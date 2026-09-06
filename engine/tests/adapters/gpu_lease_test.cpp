#include "adapters/host/gpu_lease.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace ambient::host {
namespace {

// A holder that never releases is a process wedged in a driver call: the
// bounded acquire gives up, marks the lease broken, and every later acquire
// returns at once so the waiter carries on without the GPU to itself
TEST(GpuLease, ABoundedAcquireGivesUpOnAWedgedHolderAndStaysBroken) {
    const std::string name =
        "Local\\ambient-gpu-lease-wedged-" + std::to_string(GetCurrentProcessId());
    GpuLease wedged(name);
    GpuLease waiter(name);
    const auto held = wedged.Acquire();  // never released while this test runs

    // On another thread: a mutex is recursive for the thread that owns it
    double waited = 0;
    double again = 0;
    std::thread other([&] {
        const auto t0 = std::chrono::steady_clock::now();
        const auto gave_up = waiter.Acquire(std::chrono::milliseconds(200));
        waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        EXPECT_EQ(gave_up.waited(), 0.0) << "an empty guard: nothing to release";
        const auto t1 = std::chrono::steady_clock::now();
        const auto at_once = waiter.Acquire(std::chrono::milliseconds(5000));
        again = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    });
    other.join();
    EXPECT_GE(waited, 0.15);
    EXPECT_TRUE(waiter.Broken());
    EXPECT_LT(again, 0.05) << "a broken lease never waits again";
    EXPECT_FALSE(wedged.Broken()) << "the holder's own handle is unaffected";
}

TEST(GpuLease, UnnamedLeaseIsInertAndFree) {
    GpuLease lease("");
    EXPECT_FALSE(lease.Active());
    const auto guard = lease.Acquire();
    EXPECT_EQ(guard.waited(), 0.0);
}

TEST(GpuLease, TwoHoldersOfOneNameTakeTurns) {
    const std::string name =
        "Local\\ambient-gpu-lease-test-" + std::to_string(GetCurrentProcessId());
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
