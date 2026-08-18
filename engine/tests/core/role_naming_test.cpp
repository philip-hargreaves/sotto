#include "core/role_naming.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace sotto::diar {
namespace {

RoleTurn Turn(int cluster, std::string text, std::uint64_t frames = 32000) {
    return {cluster, frames, std::move(text)};
}

TEST(LexicalDoctorScore, SelfIdentificationScoresAsTheClinician) {
    // The named failure the suppression exists for: the old rule scored
    // this utterance -0.31, below the patient's greeting
    const double doctor =
        LexicalDoctorScore("I'm Doctor Deen Mirza from GP at Hand. Nice to see you.");
    const double patient = LexicalDoctorScore("Nice to see you.");
    EXPECT_GT(doctor, patient);
    EXPECT_GT(doctor, 0.0);
}

TEST(LexicalDoctorScore, PlanSpeechIsNotPatientSpeech) {
    EXPECT_GT(LexicalDoctorScore("I'll get the form sent out"), 0.0);
    EXPECT_GT(LexicalDoctorScore("we're going to arrange a follow-up for you"), 0.0);
    EXPECT_LT(LexicalDoctorScore("I feel dizzy and my chest hurts"), 0.0);
}

TEST(LexicalDoctorScore, PhrasesMatchWholeTokensOnly) {
    // "we can" must not fire inside "we cancelled"
    EXPECT_LT(LexicalDoctorScore("we cancelled because i was unwell"),
              LexicalDoctorScore("we can start the treatment"));
}

TEST(LexicalDoctorScore, QuestionMarksAreInvisibleByConstruction) {
    EXPECT_EQ(LexicalDoctorScore("do you have any pain?"),
              LexicalDoctorScore("do you have any pain."));
    EXPECT_EQ(LexicalDoctorScore("what does that mean? is it serious?"),
              LexicalDoctorScore("what does that mean. is it serious."));
}

std::vector<RoleTurn> Consultation() {
    return {
        Turn(0, "I'm Doctor Mirza, how are you feeling today"),
        Turn(0, "have you noticed any swelling in your knee"),
        Turn(0, "I'll send you the referral form, you should hear next week"),
        Turn(1, "I've had this pain in my knee for about three weeks"),
        Turn(1, "my leg aches when I climb the stairs"),
        Turn(1, "I think I hurt it when I was running"),
    };
}

TEST(NameRoles, TheColdStartNamesDoctorAndPatient) {
    const auto result = NameRoles(Consultation(), 2);
    ASSERT_EQ(result.doctor_cluster, 0);
    EXPECT_EQ(result.patient_cluster, 1);
    EXPECT_EQ(result.role_of_cluster[0], "doctor");
    EXPECT_EQ(result.role_of_cluster[1], "patient");
    EXPECT_GE(result.margin, kRoleMinMargin);
}

TEST(NameRoles, AQuestionAskingPatientCannotInvertTheDecision) {
    auto turns = Consultation();
    for (auto& turn : turns) {  // move every question mark to the patient
        if (turn.cluster == 1) turn.text += "?";
    }
    const auto inverted = NameRoles(turns, 2);
    const auto normal = NameRoles(Consultation(), 2);
    EXPECT_EQ(inverted.doctor_cluster, normal.doctor_cluster);
    EXPECT_EQ(inverted.margin, normal.margin) << "invariant by construction, not empirically";
}

TEST(NameRoles, AThinMarginAbstainsToNumberedSpeakers) {
    const std::vector<RoleTurn> greetings{Turn(0, "good morning"), Turn(1, "good morning")};
    const auto result = NameRoles(greetings, 2);
    EXPECT_EQ(result.doctor_cluster, -1);
    EXPECT_EQ(result.role_of_cluster[0], "speaker 1");
    EXPECT_EQ(result.role_of_cluster[1], "speaker 2");
}

TEST(NameRoles, MeansDecideNotTurnCounts) {
    // Two strong clinician turns against six mild patient turns: a sum
    // would let the count vote; the mean must not
    std::vector<RoleTurn> turns{
        Turn(0, "I'll arrange the scan and I want you to rest your knee"),
        Turn(0, "you should take the tablets with your evening meal"),
    };
    for (int i = 0; i < 6; ++i) turns.push_back(Turn(1, "I see okay"));
    const auto result = NameRoles(turns, 2);
    EXPECT_EQ(result.doctor_cluster, 0);
}

TEST(NameRoles, EmptyTurnsCarryNoEvidence) {
    auto turns = Consultation();
    for (int i = 0; i < 20; ++i) turns.push_back(Turn(0, ""));
    const auto result = NameRoles(turns, 2);
    EXPECT_EQ(result.doctor_cluster, 0) << "empty turns must not dilute the cluster mean";
}

TEST(NameRoles, AThirdClusterIsNeverACandidate) {
    auto turns = Consultation();
    turns.push_back(Turn(2, "is it serious doctor", 8000));  // brief companion
    const auto result = NameRoles(turns, 3);
    EXPECT_EQ(result.role_of_cluster[2], "unknown");
    EXPECT_EQ(result.role_of_cluster[0], "doctor");
}

TEST(NameRoles, TheAnchorOutranksTheLexicalSignal) {
    // Anchor similarities name the lexically patient-looking cluster as the
    // doctor: content-blind, rank not threshold, so even a small gap decides
    const std::vector<double> sims{0.41, 0.44};
    const auto result = NameRoles(Consultation(), 2, sims);
    EXPECT_EQ(result.doctor_cluster, 1);
    EXPECT_EQ(result.role_of_cluster[1], "doctor");
    EXPECT_EQ(result.role_of_cluster[0], "patient");
}

TEST(NameRoles, ASingleClusterAbstains) {
    const std::vector<RoleTurn> turns{Turn(0, "hello there, how can I help")};
    const auto result = NameRoles(turns, 1);
    EXPECT_EQ(result.doctor_cluster, -1);
    EXPECT_EQ(result.role_of_cluster[0], "speaker 1");
}

}  // namespace
}  // namespace sotto::diar
