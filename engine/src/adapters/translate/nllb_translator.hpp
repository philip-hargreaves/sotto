#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ports/translator.hpp"

namespace sotto::models {
class ModelStore;
class OvRuntime;
}  // namespace sotto::models

namespace sotto::translate {

// NLLB-200 behind the translator port: encoder once, then a greedy decode
// over the stateful decoder. Runs on the manifest device (CPU), so it
// never touches the GPU models. Loads lazily on first use.
class NllbTranslator : public ITranslator {
   public:
    NllbTranslator(const models::ModelStore& store, models::OvRuntime& runtime);
    ~NllbTranslator() override;

    std::vector<std::string> Languages() override;

    std::string Translate(const std::string& text, const std::string& language,
                          const Progress& progress) override;

    void Cancel() override;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sotto::translate
