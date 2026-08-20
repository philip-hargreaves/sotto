#pragma once

#include <functional>
#include <string>
#include <vector>

namespace sotto::translate {

// Translates finished text to a named target language. Translate streams
// partial text through progress and returns the full translation; it
// throws on failure. Cancel interrupts from another thread.
class ITranslator {
   public:
    using Progress = std::function<void(const std::string&)>;

    virtual ~ITranslator() = default;

    virtual std::vector<std::string> Languages() = 0;

    virtual std::string Translate(const std::string& text, const std::string& language,
                                  const Progress& progress) = 0;

    virtual void Cancel() {}
};

}  // namespace sotto::translate
