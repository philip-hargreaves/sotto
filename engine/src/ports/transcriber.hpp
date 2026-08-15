#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace sotto::asr {

struct Turn {
    std::uint64_t first_frame = 0;
    std::uint64_t frame_count = 0;
    std::string speaker;  // Empty until diarisation
    std::string text;
};

// Receives finalised turns, in order, possibly on the transcriber's thread
class ITurnSink {
   public:
    virtual ~ITurnSink() = default;

    virtual void OnTurn(const Turn& turn) = 0;
};

// One session's transcription. Windows arrive in order while capture runs;
// Finish flushes pending work and blocks until every turn reached the sink.
// Nothing but Finish may assume the session is over.
class ITranscriber {
   public:
    virtual ~ITranscriber() = default;

    virtual void Begin(ITurnSink& sink) = 0;
    virtual void Submit(std::span<const float> frames, std::uint64_t first_frame) = 0;
    virtual void Finish() = 0;
};

}  // namespace sotto::asr
