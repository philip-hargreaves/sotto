#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ports/transcriber.hpp"

namespace ambient::note {

// How the note is written: structure and length, each a prompt file so a
// change needs no rebuild. Values are validated at the RPC boundary.
struct NoteOptions {
    std::string style = "prose";      // prose | soap
    std::string detail = "standard";  // concise | standard | detailed
};

// Streams partials, returns the note, throws on failure; Cancel
// interrupts from another thread
class INoteWriter {
   public:
    using Progress = std::function<void(const std::string&)>;

    virtual ~INoteWriter() = default;

    virtual std::string Write(const std::vector<asr::Turn>& transcript, const NoteOptions& options,
                              const Progress& progress) = 0;

    // Patient information from the finished note, on the same model; a
    // writer that only writes notes returns false and is still valid
    virtual bool WritesPatient() const {
        return false;
    }

    virtual std::string WritePatient(const std::string&, const Progress&) {
        return {};
    }

    // Empty means no title - the shell shows the date - so failure is never an error
    virtual std::string WriteLabel(const std::string&) {
        return {};
    }

    // Starts the background model load; idempotent
    virtual void Prepare() {}

    // An in-process writer needs the transcriber off the GPU first
    // (measured KV corruption); an out-of-process writer does not
    virtual bool WantsTranscriberReleased() const {
        return true;
    }

    virtual void Cancel() {}
};

}  // namespace ambient::note
