#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace sotto::asr {

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

    // Free the model's device memory; the next use reloads it
    virtual void Release() {}
};

}  // namespace sotto::asr
