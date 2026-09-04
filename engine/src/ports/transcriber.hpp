#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ambient::asr {

struct Turn {
    std::uint64_t first_frame = 0;
    std::uint64_t frame_count = 0;
    std::string speaker;
    std::string text;
};

// Receives finalised turns, in order, possibly on the transcriber's thread
class ITurnSink {
   public:
    virtual ~ITurnSink() = default;

    virtual void OnTurn(const Turn& turn) = 0;
};

class ITranscriber {
   public:
    virtual ~ITranscriber() = default;

    virtual void Begin(ITurnSink& sink) = 0;

    // first_new_frame marks where unheard audio begins; turns wholly before
    // it are not emitted again
    virtual void Submit(std::span<const float> frames, std::uint64_t first_frame,
                        std::uint64_t first_new_frame = 0) = 0;

    virtual void Finish() = 0;

    // Decode one clip off the live turn stream, safe mid-session; empty when
    // unsupported (a re-split then keeps the original turn)
    virtual std::string DecodeClip(std::span<const float>, std::uint64_t) {
        return {};
    }

    // The same decode as chunks with absolute frames; one chunk by default
    virtual std::vector<Turn> DecodeClipChunks(std::span<const float> frames,
                                               std::uint64_t first_frame) {
        Turn turn;
        turn.first_frame = first_frame;
        turn.frame_count = frames.size();
        turn.text = DecodeClip(frames, first_frame);
        return {turn};
    }

    // Chunk edges (absolute frames, inside the clip) from every decode since
    // the last call; AMBIENT_CLIP_CUTS feeds them back as cut points. Empty
    // when unsupported
    virtual std::vector<std::uint64_t> TakeClipCuts() {
        return {};
    }

    // Free the model's device memory; the next use reloads it
    virtual void Release() {}
};

}  // namespace ambient::asr
