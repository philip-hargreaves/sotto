#pragma once

#include <functional>
#include <string>
#include <vector>

namespace sotto::translate {

// Streams partials, returns the translation, throws on failure; Cancel
// interrupts from another thread
class ITranslator {
   public:
    using Progress = std::function<void(const std::string&)>;

    virtual ~ITranslator() = default;

    virtual std::vector<std::string> Languages() = 0;

    // Starts any slow loading in the background so the first Translate is
    // warm; safe to call repeatedly
    virtual void Prepare() {}

    virtual std::string Translate(const std::string& text, const std::string& language,
                                  const Progress& progress) = 0;

    virtual void Cancel() {}
};

}  // namespace sotto::translate
