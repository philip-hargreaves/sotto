#pragma once

#include <string>
#include <utility>
#include <vector>

#include "ports/transcriber.hpp"

namespace ambient::asr {

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

    // Clip cuts a test scripts, handed over once like the worker's
    std::vector<std::uint64_t> clip_cuts;

    std::vector<std::uint64_t> TakeClipCuts() override {
        return std::exchange(clip_cuts, {});
    }

   private:
    ITurnSink* sink_ = nullptr;
    int turns_ = 0;
};

}  // namespace ambient::asr
