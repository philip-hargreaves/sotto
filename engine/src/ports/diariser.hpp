#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace sotto::diar {

struct LabelledSlice {
    std::uint64_t first_frame = 0;
    std::uint64_t end_frame = 0;
    int cluster = 0;
};

// Who-spoke-when over a whole recording: time-sorted speech slices with
// anonymous cluster labels. Roles are a later, separate step
class IDiariser {
   public:
    virtual ~IDiariser() = default;

    virtual std::vector<LabelledSlice> Diarise(std::span<const float> audio) = 0;
};

}  // namespace sotto::diar
