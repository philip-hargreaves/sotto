#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ports/note_writer.hpp"

namespace sotto::note {

// The note model in its own supervised process, so a GPU driver fault in
// generation can never corrupt or hang the engine. Spawns the host lazily,
// forwards Prepare so the load still hides inside capture, and streams
// partials back over a private hardened pipe.
class WorkerNoteWriter : public INoteWriter {
   public:
    WorkerNoteWriter(std::filesystem::path host_exe, std::filesystem::path models_root,
                     std::filesystem::path prompt_path);
    ~WorkerNoteWriter() override;

    void Prepare() override;

    bool WritesPatient() const override {
        return true;
    }

    bool WantsTranscriberReleased() const override {
        return false;
    }

    std::string Write(const std::vector<asr::Turn>& transcript, const Progress& progress) override;

    std::string WritePatient(const std::string& note, const Progress& progress) override;

    void Cancel() override;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sotto::note
