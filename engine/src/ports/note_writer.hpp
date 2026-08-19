#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ports/transcriber.hpp"

namespace sotto::note {

// Writes the clinical note from the sealed transcript. Write streams
// partial text through progress and returns the full note; it throws on
// failure. Cancel interrupts an in-flight Write from another thread.
class INoteWriter {
   public:
    using Progress = std::function<void(const std::string&)>;

    virtual ~INoteWriter() = default;

    virtual std::string Write(const std::vector<asr::Turn>& transcript,
                              const Progress& progress) = 0;

    // Safe warm-up for a Write that may follow; never touches the GPU
    virtual void Prepare() {}

    virtual void Cancel() {}
};

}  // namespace sotto::note
