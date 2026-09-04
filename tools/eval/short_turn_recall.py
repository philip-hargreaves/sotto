# Short-turn recall: the fraction of reference turns under N words that some hypothesis turn
# of the right speaker overlaps in time. Turn-weighted and order-aware, so it sees the
# absorbed-answer error class that word attribution (word-weighted) cannot. Paired per consult.
#   python tools/eval/short_turn_recall.py evalS16 evalC16 [max_words=5]
import glob
import json
import os
import statistics
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "perf-loop"))
import score_gate  # noqa: E402

TAGS = sys.argv[1:3] if len(sys.argv) >= 3 else ["evalS16", "evalC16"]
MAX_WORDS = int(sys.argv[3]) if len(sys.argv) > 3 else 5
MIN_OVERLAP = 0.5  # of the reference turn's duration, covered by same-speaker hypothesis time


def load(tag, consult):
    path = os.path.join(score_gate.HERE, "transcripts", f"{tag}-{consult}_mixed.json")
    if not os.path.exists(path):
        return None
    turns = json.load(open(path, encoding="utf-8"))["turns"]
    return [(t["firstFrame"] / 16000.0, (t["firstFrame"] + t["frameCount"]) / 16000.0, t["speaker"])
            for t in turns if t["text"].strip()]


def recall(ref_ivs, hyp):
    hit = total = 0
    missed = []
    for start, end, spk, text in ref_ivs:
        words = score_gate.normalise(text)
        if not words or len(words) > MAX_WORDS:
            continue
        total += 1
        dur = max(1e-6, end - start)
        covered = sum(max(0.0, min(end, e) - max(start, s)) for s, e, hs in hyp if hs == spk)
        if covered / dur >= MIN_OVERLAP:
            hit += 1
        else:
            missed.append((round(start, 1), spk, text))
    return hit, total, missed


def main():
    consults = sorted(os.path.basename(p)[:-len("_doctor.TextGrid")]
                      for p in glob.glob(os.path.join(score_gate.REFS, "*_doctor.TextGrid")))
    rows = []
    for consult in consults:
        arms = {t: load(t, consult) for t in TAGS}
        if any(v is None for v in arms.values()):
            continue
        ref_ivs, _ = score_gate.reference(consult)
        row = {"consult": consult}
        for t in TAGS:
            hit, total, missed = recall(ref_ivs, arms[t])
            row[t] = {"hit": hit, "total": total, "recall": hit / max(1, total), "missed": missed}
        rows.append(row)
    if not rows:
        print("nothing scored")
        return
    a, b = TAGS
    out = os.path.join(score_gate.HERE, f"short-turn-recall-{a}-vs-{b}.json")
    json.dump(rows, open(out, "w", encoding="utf-8"), indent=1)
    for t in TAGS:
        hit = sum(r[t]["hit"] for r in rows)
        total = sum(r[t]["total"] for r in rows)
        print(f"{t:10s} short-turn recall {100.0 * hit / total:6.2f}%  ({hit}/{total} reference turns <= {MAX_WORDS} words), "
              f"median per consult {100.0 * statistics.median(r[t]['recall'] for r in rows):.1f}%")
    d = [r[b]["recall"] - r[a]["recall"] for r in rows]
    better = sum(1 for x in d if x > 0)
    worse = sum(1 for x in d if x < 0)
    print(f"paired {b} - {a}: median {100.0 * statistics.median(d):+.2f} pt; {b} higher in {better}, lower in {worse}, equal in {len(d) - better - worse}")
    print(f"per-consult JSON with missed turns: {out}")


if __name__ == "__main__":
    main()
