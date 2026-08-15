#pragma once

#include <string>

#include "ports/transcriber.hpp"

namespace sotto::asr {

// Stand-in until the Whisper adapter lands: one deterministic line per
// window, answered inline. Unmistakably fake by content
class ScriptedTranscriber : public ITranscriber {
   public:
    void Begin(ITurnSink& sink) override {
        sink_ = &sink;
        turns_ = 0;
    }

    void Submit(std::span<const float> frames, std::uint64_t first_frame) override {
        Turn turn;
        turn.first_frame = first_frame;
        turn.frame_count = frames.size();
        turn.text = "scripted turn " + std::to_string(turns_++) + ", " +
                    std::to_string(frames.size()) + " frames";
        sink_->OnTurn(turn);
    }

    void Finish() override {}

   private:
    ITurnSink* sink_ = nullptr;
    int turns_ = 0;
};

}  // namespace sotto::asr
