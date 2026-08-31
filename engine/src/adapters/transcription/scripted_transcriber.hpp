#pragma once

#include <string>

#include "ports/transcriber.hpp"

namespace sotto::asr {

// CI stand-in when no ASR model is staged
class ScriptedTranscriber : public ITranscriber {
   public:
    void Begin(ITurnSink& sink) override {
        sink_ = &sink;
        turns_ = 0;
    }

    void Submit(std::span<const float> frames, std::uint64_t first_frame,
                std::uint64_t /*first_new_frame*/ = 0) override {
        Turn turn;
        turn.first_frame = first_frame;
        turn.frame_count = frames.size();
        turn.text = "scripted turn " + std::to_string(turns_++) + ", " +
                    std::to_string(frames.size()) + " frames";
        sink_->OnTurn(turn);
    }

    void Finish() override {}

    std::string DecodeClip(std::span<const float> frames, std::uint64_t first_frame) override {
        return "re-decoded " + std::to_string(frames.size()) + " frames at " +
               std::to_string(first_frame);
    }

   private:
    ITurnSink* sink_ = nullptr;
    int turns_ = 0;
};

}  // namespace sotto::asr
