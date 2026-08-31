#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ports/note_writer.hpp"

namespace ambient::note {

// The note model in its own supervised process, so a GPU driver fault can
// never corrupt or hang the engine
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

    std::string Write(const std::vector<asr::Turn>& transcript, const NoteOptions& options,
                      const Progress& progress) override;

    std::string WritePatient(const std::string& note, const Progress& progress) override;

    std::string WriteLabel(const std::string& note) override;

    void Cancel() override;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ambient::note
