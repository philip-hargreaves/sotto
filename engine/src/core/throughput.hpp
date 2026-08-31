#pragma once

#include <cstddef>
#include <deque>

namespace sotto::core {

// Tokens per second, measured at the source: one Token() per streamed
// piece, before any notification throttling, so the figure is the model's
// real rate. Rate() is a short rolling window for live display; Average()
// is the whole stream, the number a test can hold the hardware to.
class ThroughputMeter {
   public:
    explicit ThroughputMeter(double window_seconds = 2.0) : window_(window_seconds) {}

    void Token(double now) {
        if (total_ == 0) {
            first_ = now;
        }
        last_ = now;
        ++total_;
        stamps_.push_back(now);
        Trim(now);
    }

    // (count-1)/span over what the window holds: one token is not a rate,
    // and a stalled stream decays to zero as the window empties
    double Rate(double now) {
        Trim(now);
        if (stamps_.size() < 2) {
            return 0;
        }
        const double span = now - stamps_.front();
        return span <= 0 ? 0 : static_cast<double>(stamps_.size() - 1) / span;
    }

    double Average() const {
        if (total_ < 2 || last_ <= first_) {
            return 0;
        }
        return static_cast<double>(total_ - 1) / (last_ - first_);
    }

    void Reset() {
        stamps_.clear();
        total_ = 0;
        first_ = last_ = 0;
    }

   private:
    void Trim(double now) {
        while (!stamps_.empty() && now - stamps_.front() > window_) {
            stamps_.pop_front();
        }
    }

    double window_;
    std::deque<double> stamps_;
    std::size_t total_ = 0;
    double first_ = 0;
    double last_ = 0;
};

}  // namespace sotto::core
