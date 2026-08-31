#include "core/note_label.hpp"

#include <gtest/gtest.h>

namespace ambient::core {
namespace {

TEST(NoteLabel, TakesTheFirstSentence) {
    EXPECT_EQ(LabelFrom("Swelling of the left elbow for a week. Not painful."),
              "Swelling of the left elbow for a week");
    EXPECT_EQ(LabelFrom("Is it infected? No."), "Is it infected");
}

TEST(NoteLabel, DecimalsDoNotEndIt) {
    EXPECT_EQ(LabelFrom("Ibuprofen 1.5 g daily was advised. Review in a week."),
              "Ibuprofen 1.5 g daily was advised");
    EXPECT_EQ(LabelFrom("Temperature 37.8C"), "Temperature 37.8C");
}

TEST(NoteLabel, CollapsesWhitespaceAndHeaders) {
    EXPECT_EQ(LabelFrom("Subjective:\n  Swollen   left\r\nelbow. Objective: warm."),
              "Subjective: Swollen left elbow");
    EXPECT_EQ(LabelFrom("   \n  "), "");
    EXPECT_EQ(LabelFrom(""), "");
}

TEST(NoteLabel, CutsAtAWordUnderTheCap) {
    const std::string sentence =
        "The patient, a 53-year-old male, presents with a swelling on the left elbow noticed "
        "about a week ago with no injury and no pain";
    const std::string label = LabelFrom(sentence);
    EXPECT_LE(label.size(), 80u);
    EXPECT_EQ(label, "The patient, a 53-year-old male, presents with a swelling on the left elbow");
    EXPECT_EQ(LabelFrom("abcdefghij", 5), "abcde") << "no word boundary: hard cut";
}

TEST(NoteLabel, SanitiseHoldsAModelTitleToTheListStandard) {
    EXPECT_EQ(SanitiseLabel("Elbow swelling"), "Elbow swelling");
    EXPECT_EQ(SanitiseLabel("  \"Left elbow swelling.\"  "), "Left elbow swelling");
    EXPECT_EQ(SanitiseLabel("'Chest pain follow-up',"), "Chest pain follow-up");
    EXPECT_EQ(SanitiseLabel("Diarrhoea and vomiting\nThe patient also..."),
              "Diarrhoea and vomiting");
}

TEST(NoteLabel, SanitiseRejectsWhatDoesNotSurvive) {
    EXPECT_EQ(SanitiseLabel(""), "");
    EXPECT_EQ(SanitiseLabel("  \"...\"  "), "");
    EXPECT_EQ(SanitiseLabel("\n\n"), "");
    const std::string rambling(200, 'a');
    EXPECT_LE(SanitiseLabel(rambling).size(), 60u);
}

}  // namespace
}  // namespace ambient::core
