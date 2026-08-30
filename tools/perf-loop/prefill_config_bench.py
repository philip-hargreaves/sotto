# Prefill throughput of the 9B on the iGPU under OpenVINO GenAI pipeline
# properties. One configuration per process (fresh state). Prompt: the sealed
# elbow transcript in the note format, as the host builds it.
import json
import os
import sys
import time

import openvino_genai as og
import prefix_bench as pb

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIGS = {
    "baseline (CACHE_DIR only)": {},
    "KV_CACHE_PRECISION=u8": {"KV_CACHE_PRECISION": "u8"},
    "DYNAMIC_QUANTIZATION_GROUP_SIZE=0": {"DYNAMIC_QUANTIZATION_GROUP_SIZE": "0"},
    "DYNAMIC_QUANTIZATION_GROUP_SIZE=32": {"DYNAMIC_QUANTIZATION_GROUP_SIZE": "32"},
    "PERFORMANCE_HINT=LATENCY": {"PERFORMANCE_HINT": "LATENCY"},
    "KV u8 + DQ 0": {"KV_CACHE_PRECISION": "u8", "DYNAMIC_QUANTIZATION_GROUP_SIZE": "0"},
}


def main():
    name = sys.argv[1]
    props = CONFIGS[name]
    data = json.load(open(os.path.join(HERE, "transcripts", "spec-cfull_elbow.json"), encoding="utf-8"))
    sealed_prompt = pb.prompt(data["turns"], "sealed")
    t0 = time.perf_counter()
    try:
        pipe = og.LLMPipeline(pb.MODEL, "GPU", CACHE_DIR=os.path.join(pb.MODEL, ".cache"), **props)
    except Exception as e:
        print(f"{name}: load failed: {str(e)[:120]}")
        return
    load = time.perf_counter() - t0
    tok = pipe.get_tokenizer()
    n = len(tok.encode(sealed_prompt).input_ids.data[0])
    one = og.GenerationConfig(); one.max_new_tokens = 1; one.do_sample = False; one.apply_chat_template = False
    gen = og.GenerationConfig(); gen.max_new_tokens = 8; gen.do_sample = False; gen.apply_chat_template = False
    pipe.generate(pb.SYSTEM, one)  # the host's warm
    s = pb.FirstToken()
    pipe.generate(sealed_prompt, gen, s)
    print(f"{name:<40} load {load:5.1f} s | {n} tokens | first token {s.first:.2f} s "
          f"({(n - 553) / s.first:.0f} tok/s over the transcript)")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        for k in CONFIGS:
            print(k)
    else:
        main()
