#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ports/note_writer.hpp"

namespace sotto::models {
class ModelStore;
class OvRuntime;
}  // namespace sotto::models

namespace sotto::note {

// Qwen behind the note port. The pipeline is loaded per Write and freed
// after; the prompt file is re-read per note.
class QwenNoteWriter : public INoteWriter {
   public:
    QwenNoteWriter(const models::ModelStore& store, models::OvRuntime& runtime,
                   std::filesystem::path prompt_path);
    ~QwenNoteWriter() override;

    std::string Write(const std::vector<asr::Turn>& transcript, const Progress& progress) override;

    // Reads the weights back into the file cache, off the GPU
    void Prepare() override;

    void Cancel() override;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sotto::note
