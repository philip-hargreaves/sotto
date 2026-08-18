#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

namespace sotto::diar {

// Below this lexical margin no one is named. Chosen from the observed
// margin range, not swept; revisit first if it abstains too often
inline constexpr double kRoleMinMargin = 0.5;

struct RoleTurn {
    int cluster = 0;
    std::uint64_t frame_count = 0;
    std::string text;
};

struct RoleResult {
    std::vector<std::string> role_of_cluster;  // doctor | patient | speaker N | unknown
    int doctor_cluster = -1;                   // -1: abstained
    int patient_cluster = -1;
    double margin = 0.0;
};

namespace detail {

// Lowercase tokens, apostrophes kept, so "i'm" and "we'll" compare exactly
inline std::vector<std::string> WordsOf(const std::string& text) {
    std::vector<std::string> words;
    std::string word;
    for (const char raw : text) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(raw)));
        if ((c >= 'a' && c <= 'z') || c == '\'') {
            word.push_back(c);
        } else if (!word.empty()) {
            words.push_back(word);
            word.clear();
        }
    }
    if (!word.empty()) words.push_back(word);
    return words;
}

inline bool In(const std::vector<std::string>& set, const std::string& word) {
    return std::find(set.begin(), set.end(), word) != set.end();
}

// Consecutive whole tokens, never substrings: "we can" must not fire
// inside "we cancelled"
inline bool HasPhrase(const std::vector<std::string>& words,
                      const std::vector<std::string>& phrase) {
    if (phrase.empty() || words.size() < phrase.size()) return false;
    for (std::size_t i = 0; i + phrase.size() <= words.size(); ++i) {
        std::size_t j = 0;
        while (j < phrase.size() && words[i + j] == phrase[j]) ++j;
        if (j == phrase.size()) return true;
    }
    return false;
}

// The clinician naming themselves; "dr" and "doctor" both, ASR output varies
inline const std::vector<std::vector<std::string>> kIdentPhrases = {
    {"i'm", "dr"},
    {"i'm", "doctor"},
    {"i", "am", "dr"},
    {"i", "am", "doctor"},
    {"this", "is", "dr"},
    {"this", "is", "doctor"},
    {"my", "name", "is", "dr"},
    {"my", "name", "is", "doctor"},
    {"from", "gp", "at", "hand"},
    {"calling", "from"},
};

// Plan speech is all first-person; unnamed, the penalty would score the
// doctor's most characteristic speech as the patient's
inline const std::vector<std::vector<std::string>> kPlanPhrases = {
    {"i'll"},
    {"i", "will"},
    {"i'd", "like"},
    {"i", "would"},
    {"i'm", "going", "to"},
    {"i", "want", "you", "to"},
    {"we'll"},
    {"we", "will"},
    {"we're", "going", "to"},
    {"we", "can"},
    {"let's"},
};

}  // namespace detail

// Cold-start scorer, positive leaning clinician. Reads no question
// features: question weighting inverts on consultations where the patient
// asks the questions (measured 6/6 -> 0/6). Self-identification and plan
// speech suppress the first-person penalty - the suppression is the point
inline double LexicalDoctorScore(const std::string& text) {
    const auto words = detail::WordsOf(text);
    if (words.empty()) return 0.0;
    const double n = static_cast<double>(words.size());

    static const std::vector<std::string> kFirst = {"i", "my", "me", "i'm", "i've"};
    static const std::vector<std::string> kSecond = {"you", "your", "you're"};
    double first = 0.0, second = 0.0;
    for (const auto& word : words) {
        if (detail::In(kFirst, word)) ++first;
        if (detail::In(kSecond, word)) ++second;
    }

    double bonus = 0.0;
    for (const auto& phrase : detail::kIdentPhrases) {
        if (detail::HasPhrase(words, phrase)) {
            bonus += 8.0;
            first = 0.0;
            break;
        }
    }
    int plan = 0;
    for (const auto& phrase : detail::kPlanPhrases) {
        if (detail::HasPhrase(words, phrase)) ++plan;
    }
    if (plan > 0) {
        first = std::max(0.0, first - plan);
        bonus += 2.5 * std::min(plan, 3);  // capped: one plan-heavy turn cannot dominate
    }
    return bonus + 7.0 * (second / n) - 11.0 * (first / n);
}

// Anonymous clusters -> roles. The two dominant clusters by talk time are
// the candidates; anything further stays unknown. With anchor similarities
// the nearer of the pair is the doctor (rank, never a threshold). Without
// them the higher mean lexical score decides, abstaining below the margin:
// a confident inversion is the one failure that corrupts the record
inline RoleResult NameRoles(const std::vector<RoleTurn>& turns, int cluster_count,
                            const std::vector<double>& anchor_similarity = {}) {
    RoleResult result;
    if (cluster_count < 1) return result;
    result.role_of_cluster.assign(static_cast<std::size_t>(cluster_count), "unknown");

    std::vector<double> talk(static_cast<std::size_t>(cluster_count), 0.0);
    for (const auto& turn : turns) {
        if (turn.cluster >= 0 && turn.cluster < cluster_count) {
            talk[static_cast<std::size_t>(turn.cluster)] += static_cast<double>(turn.frame_count);
        }
    }
    std::vector<int> order(static_cast<std::size_t>(cluster_count));
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return talk[static_cast<std::size_t>(a)] > talk[static_cast<std::size_t>(b)];
    });
    const int top1 = order[0];
    const int top2 = cluster_count > 1 ? order[1] : order[0];

    int doctor;
    if (anchor_similarity.size() == static_cast<std::size_t>(cluster_count) && top1 != top2) {
        const double sim1 = anchor_similarity[static_cast<std::size_t>(top1)];
        const double sim2 = anchor_similarity[static_cast<std::size_t>(top2)];
        result.margin = std::abs(sim1 - sim2);
        doctor = sim1 >= sim2 ? top1 : top2;  // relative match; no abstention
    } else {
        double sum1 = 0.0, sum2 = 0.0;
        int n1 = 0, n2 = 0;
        for (const auto& turn : turns) {
            if (turn.text.empty()) continue;  // no lexical evidence; would dilute the mean
            const double score = LexicalDoctorScore(turn.text);
            if (turn.cluster == top1) {
                sum1 += score;
                ++n1;
            }
            if (turn.cluster == top2) {
                sum2 += score;
                ++n2;
            }
        }
        // Means, not sums: turn counts are near 50/50 and carry no signal
        const double mean1 = n1 > 0 ? sum1 / n1 : 0.0;
        const double mean2 = n2 > 0 ? sum2 / n2 : 0.0;
        result.margin = std::abs(mean1 - mean2);
        if (result.margin < kRoleMinMargin || top1 == top2) {
            // Numbered, not unknown: the separation is still certain, only
            // the roles are not
            result.role_of_cluster[static_cast<std::size_t>(top1)] = "speaker 1";
            if (top2 != top1) result.role_of_cluster[static_cast<std::size_t>(top2)] = "speaker 2";
            return result;
        }
        doctor = mean1 >= mean2 ? top1 : top2;
    }

    const int patient = doctor == top1 ? top2 : top1;
    result.doctor_cluster = doctor;
    result.patient_cluster = patient;
    result.role_of_cluster[static_cast<std::size_t>(doctor)] = "doctor";
    if (patient != doctor) result.role_of_cluster[static_cast<std::size_t>(patient)] = "patient";
    return result;
}

}  // namespace sotto::diar
