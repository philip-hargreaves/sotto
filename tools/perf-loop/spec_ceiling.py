# Ceiling of cache-based speculative prefill on one consult. Reads the
# engine's `specturn` measurement lines (final cluster, cache hit/miss, text
# per merged turn, in time order) and the sealed transcript, builds the
# speculative prompt a note host would have received 0.3 s into finalise
# (cluster letters, texts up to the first cache miss) and the sealed prompt
# in the same format, then measures shared tokens and first-token time.
import json
import os
import re
import sys
import time

import openvino_genai as og
import prefix_bench as pb

HERE = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.join(HERE, sys.argv[1] if len(sys.argv) > 1 else "spec-engine.log")
SEALED = os.path.join(HERE, "transcripts", "spec-cfull_elbow.json")

turns = []
for line in open(LOG, encoding="utf-8", errors="replace"):
    m = re.match(r"sotto-engine: specturn (\d+) (\d+) (hit|miss) ([\d.]+) (.*)$", line.rstrip("\n"))
    if m:
        turns.append({"i": int(m.group(1)), "cluster": int(m.group(2)), "hit": m.group(3) == "hit",
                      "at": float(m.group(4)), "text": m.group(5)})
turns = [t for t in turns if t["text"]]  # turns without text never enter the prompt
sealed = json.load(open(SEALED, encoding="utf-8"))["turns"]
first_miss = next((k for k, t in enumerate(turns) if not t["hit"]), len(turns))
letters = "ABCD"
print(f"{len(turns)} merged turns with text; first cache miss at turn {first_miss} "
      f"({turns[first_miss]['at'] if first_miss < len(turns) else 0:.1f} s of "
      f"{turns[-1]['at']:.1f} s); hits {sum(t['hit'] for t in turns)}, misses {sum(not t['hit'] for t in turns)}")

# Role names come last, so the transcript body is label-stable; the sealed
# prompt appends who each letter was
roles = {}
for s in sealed:
    for t in turns:
        if abs(t["at"] - s["firstFrame"] / 16000) < 0.05:
            roles.setdefault(t["cluster"], s["speaker"])
trailer = "\nSPEAKER " + " and ".join(
    f"{letters[c]} is the {roles.get(c, 'unknown')}" for c in sorted({t['cluster'] for t in turns}))


def body(ts):
    return "".join(f"SPEAKER {letters[t['cluster']]}: {t['text']}\n" for t in ts)


spec_prompt = pb.SYSTEM + body(turns[:first_miss])  # what is known before the tail decode
sealed_prompt = pb.SYSTEM + body(turns) + trailer + "\n" + pb.DETAIL

t0 = time.perf_counter()
pipe = og.LLMPipeline(pb.MODEL, "GPU", CACHE_DIR=os.path.join(pb.MODEL, ".cache"))
tok = pipe.get_tokenizer()
print(f"loaded in {time.perf_counter() - t0:.1f} s")
shared, n_spec, n_sealed = pb.common_prefix(tok, spec_prompt, sealed_prompt)
print(f"shared prefix {shared} of {n_sealed} sealed tokens ({shared / n_sealed * 100:.0f} %); "
      f"instruction block alone is {pb.common_prefix(tok, pb.SYSTEM, sealed_prompt)[0]}")

one = og.GenerationConfig(); one.max_new_tokens = 1; one.do_sample = False; one.apply_chat_template = False
gen = og.GenerationConfig(); gen.max_new_tokens = 8; gen.do_sample = False; gen.apply_chat_template = False


def first_token(p):
    s = pb.FirstToken()
    pipe.generate(p, gen, s)
    return s.first


# One condition per process: the stateful pipeline keeps KV beyond the common
# prefix, so a second run on the same prompt is never cold
mode = sys.argv[2] if len(sys.argv) > 2 else "cold"
pipe.generate(pb.SYSTEM, one)  # the host's instruction-block warm
if mode == "spec":
    t = time.perf_counter()
    pipe.generate(spec_prompt, one)
    print(f"speculative prefill {time.perf_counter() - t:.2f} s (would overlap the tail decode)")
print(f"[{mode}] first token on the sealed prompt: {first_token(sealed_prompt):.2f} s")
