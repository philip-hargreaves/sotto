# Readability mechanism statistics for two perf-loop sweeps:
# broken-bigram percentage against the reference, turn shape, duplicated spans, and
# turn-level identity between the two arms. Paired per consult; prints a markdown table
# and writes JSON beside the transcripts.
#   python tools/eval/readability_stats.py evalS16 evalC16
import glob
import json
import os
import statistics
import sys
from collections import Counter

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "perf-loop"))
import score_gate  # noqa: E402  (normalise, reference, REFS, HERE)

TAGS = sys.argv[1:3] if len(sys.argv) >= 3 else ["evalS16", "evalC16"]
OUT = os.path.join(score_gate.HERE, f"readability-{TAGS[0]}-vs-{TAGS[1]}.json")


def load(tag, consult):
    path = os.path.join(score_gate.HERE, "transcripts", f"{tag}-{consult}_mixed.json")
    if not os.path.exists(path):
        return None
    return json.load(open(path, encoding="utf-8"))["turns"]


def broken_bigrams(ref_words, hyp_words):
    # Adjacent displayed word pairs absent from the reference (multiset). Fires on
    # scrambling and fragment stitching; read as a delta between arms, never absolute
    if len(hyp_words) < 2:
        return 0.0
    ref_bg = Counter(zip(ref_words, ref_words[1:]))
    bad = 0
    for bg in zip(hyp_words, hyp_words[1:]):
        if ref_bg[bg] > 0:
            ref_bg[bg] -= 1
        else:
            bad += 1
    return 100.0 * bad / (len(hyp_words) - 1)


def duplicated_spans(turns):
    # A 4-gram that recurs within 10 s of audio: the fingerprint of overlap stitching
    seen = {}
    dup = 0
    for t in turns:
        words = score_gate.normalise(t["text"])
        at = t["firstFrame"] / 16000.0
        for i in range(len(words) - 3):
            key = tuple(words[i:i + 4])
            if key in seen and at - seen[key] <= 10.0:
                dup += 1
            seen[key] = at
    return dup


def shape(turns, audio_seconds):
    lengths = [len(score_gate.normalise(t["text"])) for t in turns]
    lengths = [n for n in lengths if n > 0]
    ends = sum(1 for t in turns if t["text"].rstrip()[-1:] in ".?!")
    return {
        "turns": len(turns),
        "turns_per_min": 60.0 * len(turns) / max(1.0, audio_seconds),
        "median_words": statistics.median(lengths) if lengths else 0,
        "short_frac": sum(1 for n in lengths if n < 3) / max(1, len(lengths)),
        "sentence_end_frac": ends / max(1, len(turns)),
        "duplicates": duplicated_spans(turns),
    }


def identity(a, b):
    ka = [(t["firstFrame"], t["frameCount"], t["speaker"], t["text"]) for t in a]
    kb = [(t["firstFrame"], t["frameCount"], t["speaker"], t["text"]) for t in b]
    if ka == kb:
        return "identical"
    if len(ka) == len(kb) and all(x[2] != y[2] and x[3] == y[3] and x[:2] == y[:2] for x, y in zip(ka, kb)):
        return "role flip"
    diff = sum(1 for x, y in zip(ka, kb) if x != y) + abs(len(ka) - len(kb))
    return "1-2 turns differ" if diff <= 2 else "3+ turns differ"


def main():
    consults = sorted(os.path.basename(p)[:-len("_doctor.TextGrid")]
                      for p in glob.glob(os.path.join(score_gate.REFS, "*_doctor.TextGrid")))
    rows = []
    for consult in consults:
        arms = {tag: load(tag, consult) for tag in TAGS}
        if any(v is None for v in arms.values()):
            continue
        ref_ivs, ref_words = score_gate.reference(consult)
        ref_plain = [w for w, _ in ref_words]
        audio_s = max(e for _, e, _, _ in ref_ivs)
        row = {"consult": consult, "identity": identity(arms[TAGS[0]], arms[TAGS[1]])}
        for tag in TAGS:
            hyp = [w for t in arms[tag] for w in score_gate.normalise(t["text"])]
            row[tag] = {"broken_bigram_pct": broken_bigrams(ref_plain, hyp), **shape(arms[tag], audio_s)}
        rows.append(row)
    if not rows:
        print("no consults scored yet")
        return
    json.dump(rows, open(OUT, "w", encoding="utf-8"), indent=1)

    def med(tag, key):
        return statistics.median(r[tag][key] for r in rows)

    a, b = TAGS
    print(f"{len(rows)} consults, {a} vs {b}\n")
    print("| metric (median over consults) | " + a + " | " + b + " |")
    print("|---|---|---|")
    for key, label in [("broken_bigram_pct", "broken bigrams %"), ("turns_per_min", "turns per minute"),
                       ("median_words", "median words per turn"), ("short_frac", "turns under 3 words"),
                       ("sentence_end_frac", "turns ending in . ? !"), ("duplicates", "duplicated 4-gram spans")]:
        print(f"| {label} | {med(a, key):.3g} | {med(b, key):.3g} |")
    bb = [r[b]["broken_bigram_pct"] - r[a]["broken_bigram_pct"] for r in rows]
    better = sum(1 for d in bb if d < 0)
    worse = sum(1 for d in bb if d > 0)
    print(f"\nbroken-bigram delta ({b} - {a}): median {statistics.median(bb):+.2f} pt, "
          f"fewer in {better}, more in {worse}, equal in {len(bb) - better - worse}")
    ident = Counter(r["identity"] for r in rows)
    print("identity " + a + " vs " + b + ": " + ", ".join(f"{k}: {v}" for k, v in ident.most_common()))
    print(f"\nper-consult JSON: {OUT}")


if __name__ == "__main__":
    main()
