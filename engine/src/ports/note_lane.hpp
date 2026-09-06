#pragma once

#include <functional>
#include <string>

namespace ambient::note {

// Which tier is configured and whether its model is resident;
// transitions feed the shell's status
struct NoteModelState {
    enum class Phase { kIdle, kLoading, kReady, kFailed };

    Phase phase = Phase::kIdle;
    std::string tier = "default";
    std::string id;          // the model the tier resolves to; empty when none is staged
    std::string name;        // its display name
    std::string detail;      // the reason, on kFailed
    double seconds = 0;      // verify + load, on kReady
    bool first_use = false;  // no compile cache yet: a load that takes minutes
};

const char* PhaseName(NoteModelState::Phase phase);

// The note lane's configuration surface: a tier is a role the model store
// resolves, never a model. Separate from INoteWriter so a writer that
// serves one model needs none of this
class INoteLane {
   public:
    using Listener = std::function<void(const NoteModelState&)>;

    virtual ~INoteLane() = default;

    // Idempotent. A different tier ends the resident model and loads the
    // new one at once. Throws std::invalid_argument when no note model
    // claims the tier, naming what is staged
    virtual NoteModelState Configure(const std::string& tier) = 0;

    virtual NoteModelState State() const = 0;

    // Called on every transition, off the caller's thread
    virtual void SetListener(Listener listener) = 0;
};

inline const char* PhaseName(NoteModelState::Phase phase) {
    switch (phase) {
        case NoteModelState::Phase::kLoading:
            return "loading";
        case NoteModelState::Phase::kReady:
            return "ready";
        case NoteModelState::Phase::kFailed:
            return "failed";
        default:
            return "idle";
    }
}

}  // namespace ambient::note
