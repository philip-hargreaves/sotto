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

namespace sotto::metrics {
class Registry;
}  // namespace sotto::metrics

namespace sotto::note {

// Qwen behind the note port. Prepare starts one background load and the
// pipeline stays resident; the prompt file is re-read per note.
class QwenNoteWriter : public INoteWriter {
   public:
    QwenNoteWriter(const models::ModelStore& store, models::OvRuntime& runtime,
                   std::filesystem::path prompt_path, metrics::Registry* metrics = nullptr);
    ~QwenNoteWriter() override;

    std::string Write(const std::vector<asr::Turn>& transcript, const Progress& progress) override;

    bool WritesPatient() const override {
        return true;
    }

    std::string WritePatient(const std::string& note, const Progress& progress) override;

    // Loads the pipeline in the background; idempotent, retried on failure
    void Prepare() override;

    void Cancel() override;

   private:
    std::string Generate(const std::string& prompt, const Progress& progress);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sotto::note
