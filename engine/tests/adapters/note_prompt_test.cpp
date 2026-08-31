#include "adapters/note/note_prompt.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace ambient::note {
namespace {

TEST(NotePrompt, LoadsTheFileVerbatim) {
    const auto path = std::filesystem::temp_directory_path() / "ambient-note-prompt-test.md";
    std::ofstream(path) << "Write the note.\n\nTRANSCRIPT:\n";
    EXPECT_EQ(LoadPrompt(path), "Write the note.\n\nTRANSCRIPT:\n");
    std::filesystem::remove(path);
}

TEST(NotePrompt, AMissingFileIsALoudFailure) {
    EXPECT_THROW(LoadPrompt("C:/does/not/exist/prompt.md"), std::runtime_error);
}

TEST(NotePrompt, TranscriptBlockIsUpperCasedRolesOnePerLine) {
    const std::vector<asr::Turn> turns{{0, 100, "doctor", "How long has it hurt?"},
                                       {100, 100, "patient", "About a week."},
                                       {200, 100, "", "Okay."}};
    EXPECT_EQ(TranscriptBlock(turns),
              "DOCTOR: How long has it hurt?\n"
              "PATIENT: About a week.\n"
              "SPEAKER: Okay.\n");
}

}  // namespace
}  // namespace ambient::note
