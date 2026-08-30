# Speculative prefill study on one consult: how much of the note prompt's KV
# can be reused at seal if the live transcript was prefilled at stop, under
# three transcript formats, and what the stateful pipeline gives back in
# first-token time. Mirrors the note host: same model, device, cache dir,
# raw prompt (no chat template), one-token warm of the instruction block.
import json
import os
import sys
import time

import openvino_genai as og

MODEL = r"C:\dev\sotto\models\qwen3.5-9b-int4"
PROMPTS = r"C:\dev\sotto\prompts"
HERE = os.path.dirname(os.path.abspath(__file__))
CAPTURE = os.path.join(HERE, "transcripts", sys.argv[1] if len(sys.argv) > 1 else "prefill-cfull_elbow.json")


def load_prompt(name):
    return open(os.path.join(PROMPTS, name), encoding="utf-8").read()


SYSTEM = "<|im_start|>user\n" + load_prompt("note-narrative.md")
DETAIL = load_prompt("detail-standard.md")


def block(turns, labels):
    # labels: "sealed" (DOCTOR/PATIENT/SPEAKER as stored), "none" (no labels), or a
    # list of label strings per turn
    out = []
    for i, t in enumerate(turns):
        if labels == "none":
            out.append(t["text"])
        else:
            label = (t["speaker"] or "speaker") if labels == "sealed" else labels[i]
            out.append(label.upper() + ": " + t["text"])
    return "\n".join(out) + "\n"


def prompt(turns, labels, trailer=""):
    return SYSTEM + block(turns, labels) + trailer + "\n" + DETAIL


def relabel_by_overlap(live, sealed):
    # The upper bound for a label-stable format: each live turn gets the sealed
    # speaker that covers most of its span (perfect speculative naming)
    labels = []
    for t in live:
        a, b = t["firstFrame"], t["firstFrame"] + t["frameCount"]
        share = {}
        for s in sealed:
            sa, sb = s["firstFrame"], s["firstFrame"] + s["frameCount"]
            ov = max(0, min(b, sb) - max(a, sa))
            if ov:
                share[s["speaker"]] = share.get(s["speaker"], 0) + ov
        labels.append(max(share, key=share.get) if share else "speaker")
    return labels


def common_prefix(tok, a, b):
    ia = tok.encode(a).input_ids.data[0].tolist()
    ib = tok.encode(b).input_ids.data[0].tolist()
    n = 0
    while n < min(len(ia), len(ib)) and ia[n] == ib[n]:
        n += 1
    return n, len(ia), len(ib)


class FirstToken:
    def __init__(self):
        self.t0 = time.perf_counter()
        self.first = None
        self.count = 0

    def __call__(self, subword):
        if self.first is None:
            self.first = time.perf_counter() - self.t0
        self.count += 1
        return og.StreamingStatus.RUNNING if self.count < 8 else og.StreamingStatus.STOP


def main():
    data = json.load(open(CAPTURE, encoding="utf-8"))
    live, sealed = data["live"], data["turns"]
    print(f"{os.path.basename(CAPTURE)}: {len(live)} live turns at stop, {len(sealed)} sealed")

    t0 = time.perf_counter()
    pipe = og.LLMPipeline(MODEL, "GPU", CACHE_DIR=os.path.join(MODEL, ".cache"))
    tok = pipe.get_tokenizer()
    print(f"loaded in {time.perf_counter() - t0:.1f} s")
    one = og.GenerationConfig()
    one.max_new_tokens = 1
    one.do_sample = False
    one.apply_chat_template = False
    gen = og.GenerationConfig()
    gen.max_new_tokens = 8
    gen.do_sample = False
    gen.apply_chat_template = False

    def warm():
        pipe.generate(SYSTEM, one)

    def first_token(p):
        s = FirstToken()
        pipe.generate(p, gen, s)
        return s.first

    sealed_labels = [t["speaker"] for t in sealed]
    scenarios = {
        "today: live SPEAKER: vs sealed DOCTOR/PATIENT":
            (prompt(live, "sealed"), prompt(sealed, "sealed")),
        "label-stable upper bound: live relabelled by overlap":
            (prompt(live, relabel_by_overlap(live, sealed)), prompt(sealed, "sealed")),
        "no labels in transcript, roles in a trailer":
            (prompt(live, "none", "\nSpeakers: unattributed"),
             prompt(sealed, "none", "\nSpeakers: " + ", ".join(
                 f"turn {i + 1} {l}" for i, l in enumerate(sealed_labels)))),
    }

    warm()
    cold = [first_token(prompt(sealed, "sealed")) for _ in range(2)]
    print(f"\ncold first token (system prefix only, today's behaviour): "
          f"{min(cold):.2f} s  (2 runs: {', '.join(f'{c:.2f}' for c in cold)})")

    print(f"\n{'scenario':<52}{'shared':>8}{'live':>7}{'sealed':>8}{'spec':>7}{'first':>7}{'gain':>7}")
    for name, (live_p, sealed_p) in scenarios.items():
        warm()
        shared, n_live, n_sealed = common_prefix(tok, live_p, sealed_p)
        t = time.perf_counter()
        pipe.generate(live_p, one)  # the speculative prefill, during finalise
        spec = time.perf_counter() - t
        first = first_token(sealed_p)
        print(f"{name:<52}{shared:>8}{n_live:>7}{n_sealed:>8}{spec:>7.2f}{first:>7.2f}{min(cold) - first:>7.2f}")
    print("\nshared/live/sealed = tokens; spec = time of the speculative prefill (hidden inside "
          "finalise); first = first-token time at seal after it; gain vs cold.")


if __name__ == "__main__":
    main()
