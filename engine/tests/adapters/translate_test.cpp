#include <gtest/gtest.h>

#include <filesystem>

#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "adapters/translate/nllb_translator.hpp"

namespace ambient::translate {
namespace {

const std::filesystem::path kModels = AMBIENT_MODELS_DIR;

constexpr const char* kSheet =
    "Your appointment today\n"
    "You came to see us about a swelling on your left elbow. "
    "It started about a week ago. It is not painful, but it feels warm.\n"
    "Your treatment and next steps\n"
    "Take ibuprofen 400mg twice a day, after food. Stop taking it if you get heartburn.";

TEST(NllbTranslator, TranslatesTheSheetAndStreams) {
    if (!std::filesystem::exists(kModels / "nllb-200-600m-int8")) {
        GTEST_SKIP() << "translation model not staged";
    }
    models::ModelStore store(kModels);
    models::OvRuntime runtime;
    NllbTranslator translator(store, runtime);

    EXPECT_FALSE(translator.Languages().empty());

    int partials = 0;
    const std::string french =
        translator.Translate(kSheet, "French", [&partials](const std::string&) { partials++; });

    ASSERT_FALSE(french.empty());
    EXPECT_NE(french, kSheet);
    EXPECT_GT(partials, 2);
    EXPECT_NE(french.find("ibuprof"), std::string::npos) << french;

    const std::string polish = translator.Translate(kSheet, "Polish", nullptr);
    ASSERT_FALSE(polish.empty());
    EXPECT_NE(polish, french);
}

TEST(NllbTranslator, AnUnknownLanguageIsRefused) {
    if (!std::filesystem::exists(kModels / "nllb-200-600m-int8")) {
        GTEST_SKIP() << "translation model not staged";
    }
    models::ModelStore store(kModels);
    models::OvRuntime runtime;
    NllbTranslator translator(store, runtime);
    EXPECT_THROW(translator.Translate("hello", "Klingon", nullptr), std::runtime_error);
}

}  // namespace
}  // namespace ambient::translate
