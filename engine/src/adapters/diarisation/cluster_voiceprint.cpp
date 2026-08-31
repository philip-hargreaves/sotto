#include "adapters/diarisation/cluster_voiceprint.hpp"

#include <algorithm>

namespace ambient::diar {

std::vector<float> ClusterVoiceprint(SpeakerEmbedder& embedder, std::span<const float> audio,
                                     const std::vector<LabelledSlice>& slices, int cluster) {
    const auto ranges = VoiceprintRanges(slices, cluster);
    if (ranges.empty()) return {};
    std::vector<float> clip;
    for (const auto& range : ranges) {
        const auto first = static_cast<std::size_t>(range.first_frame);
        const auto end =
            std::min<std::size_t>(static_cast<std::size_t>(range.end_frame), audio.size());
        if (end > first) clip.insert(clip.end(), audio.begin() + first, audio.begin() + end);
    }
    if (clip.size() < kVoiceprintMinFrames) return {};
    return embedder.Embed(clip);
}

}  // namespace ambient::diar
