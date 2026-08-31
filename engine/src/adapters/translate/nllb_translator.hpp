#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ports/translator.hpp"

namespace ambient::models {
class ModelStore;
class OvRuntime;
}  // namespace ambient::models

namespace ambient::translate {

// NLLB-200 on the manifest CPU: encoder once, greedy stateful decode;
// loads on Prepare, else on first use
class NllbTranslator : public ITranslator {
   public:
    NllbTranslator(const models::ModelStore& store, models::OvRuntime& runtime);
    ~NllbTranslator() override;

    std::vector<std::string> Languages() override;

    void Prepare() override;

    std::string Translate(const std::string& text, const std::string& language,
                          const Progress& progress) override;

    void Cancel() override;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ambient::translate
