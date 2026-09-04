# Pairwise diff judging: two sweeps that differ by one lever change a few turns per
# consult. Each differing region is shown with the reference for that window and both
# arms' text, sides randomised per item; a reader says which is closer (or same) and
# whether the difference is clinical. Sees filler drops and single-word fixes that WER cannot.
#   python tools/eval/diff_pairs.py pack <tagA> <tagB> [per_pack=35]
#   python tools/eval/diff_pairs.py merge <tagA> <tagB>
import difflib
import glob
import json
import os
import random
import re
import sys

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build", "perf-loop")
TR = os.path.join(HERE, "transcripts")
REFS = "C:/dev/intelliscribe/bench/transcription/references"
OUT = os.path.join(HERE, "diff-pairs")
LINE = re.compile(r"^\[\s*([\d.]+)s\] (\w+)\s+(.*)$")

INSTRUCTIONS = """You are comparing two automatic transcripts of UK primary-care consultations against a
human reference. Each item is one region where the two transcripts differ. REF shows the
reference turns in that window (speaker, start time, text). A and B show the two transcripts'
turns for the same window. Sides are randomised per item; you do not know which system is which.

Judge as a professional transcriber would: dropped fillers ("um", "like", "I guess", repeated
"yeah yeah") do NOT count against a side. What matters: words with content present and correct,
answers and questions present, attributed to the right speaker, readable.

For each item give:
  closer    "A", "B" or "same"  (which side is closer to the reference by the rule above; "same"
            when the difference is filler-only, punctuation-only or an equal trade)
  clinical  true if the difference changes clinical content a note writer would use
            (a symptom, negation, medication, dose, duration, decision), else false
  why       ten words

Write ONLY a JSON array of objects {"id": <id>, "closer": ..., "clinical": ..., "why": ...} to
the results path given, in item order. Do not read any other files."""


def load(tag, consult):
    turns = []
    for line in open(os.path.join(TR, f"{tag}-{consult}_mixed.txt"), encoding="utf-8", errors="replace"):
        m = LINE.match(line.rstrip("\n"))
        if m:
            turns.append((float(m.group(1)), m.group(2), m.group(3)))
    return turns


def ref_window(consult, lo, hi):
    d = json.load(open(os.path.join(REFS, f"{consult}.json"), encoding="utf-8"))
    rows = []
    for who in ("doctor", "patient"):
        for t in d[who]:
            if t["end"] > lo - 0.5 and t["start"] < hi + 0.5:
                rows.append((t["start"], who, t["text"]))
    rows.sort()
    return rows


def regions(a, b):
    sm = difflib.SequenceMatcher(a=[f"{s} {t}" for _, s, t in a], b=[f"{s} {t}" for _, s, t in b], autojunk=False)
    for op, i1, i2, j1, j2 in sm.get_opcodes():
        if op == "equal":
            continue
        ta = a[i1:i2]
        tb = b[j1:j2]
        times = [t for t, _, _ in ta + tb]
        # a pure insertion/deletion still has a window: the neighbours
        if not times:
            continue
        lo, hi = min(times), max(times)
        ends = [a[i2][0] if i2 < len(a) else lo + 5, b[j2][0] if j2 < len(b) else lo + 5]
        yield lo, max(hi, min(ends)), ta, tb


def pack(tag_a, tag_b, per_pack):
    rng = random.Random(f"{tag_a}|{tag_b}")
    items = []
    for f in sorted(glob.glob(os.path.join(TR, f"{tag_a}-*_mixed.txt"))):
        consult = os.path.basename(f)[len(tag_a) + 1:-len("_mixed.txt")]
        if not os.path.exists(os.path.join(TR, f"{tag_b}-{consult}_mixed.txt")):
            continue
        a = load(tag_a, consult)
        b = load(tag_b, consult)
        for lo, hi, ta, tb in regions(a, b):
            flip = rng.random() < 0.5
            items.append({"consult": consult, "lo": lo, "hi": hi, "flip": flip,
                          "ref": ref_window(consult, lo, hi),
                          "A": tb if flip else ta, "B": ta if flip else tb})
    os.makedirs(OUT, exist_ok=True)
    key = f"{tag_a}-vs-{tag_b}"
    json.dump(items, open(os.path.join(OUT, f"{key}-items.json"), "w", encoding="utf-8"), indent=1)
    n = 0
    for start in range(0, len(items), per_pack):
        n += 1
        lines = [INSTRUCTIONS, "", f"PACK {n}", ""]
        for k, it in enumerate(items[start:start + per_pack], start + 1):
            lines.append(f"[{k}] {it['consult']} {it['lo']:.1f}-{it['hi']:.1f} s")
            lines.append("  REF:")
            for t, who, text in it["ref"]:
                lines.append(f"    {who:7s} {t:6.1f}  {text}")
            for side in ("A", "B"):
                lines.append(f"  {side}:")
                if not it[side]:
                    lines.append("    (no turn)")
                for t, who, text in it[side]:
                    lines.append(f"    {who:7s} {t:6.1f}  {text}")
            lines.append("")
        open(os.path.join(OUT, f"{key}-pack{n:02d}.txt"), "w", encoding="utf-8", newline="\n").write("\n".join(lines))
    print(f"{key}: {len(items)} differing regions in {n} packs -> {OUT}")


def merge(tag_a, tag_b):
    key = f"{tag_a}-vs-{tag_b}"
    items = json.load(open(os.path.join(OUT, f"{key}-items.json"), encoding="utf-8"))
    counts = {tag_a: 0, tag_b: 0, "same": 0}
    clin = {tag_a: 0, tag_b: 0}
    wins = []
    for f in sorted(glob.glob(os.path.join(OUT, f"{key}-pack*-result.json"))):
        for r in json.load(open(f, encoding="utf-8")):
            it = items[int(r["id"]) - 1]
            c = r["closer"]
            if c == "same":
                counts["same"] += 1
                continue
            tag = (tag_b if (c == "A") == it["flip"] else tag_a) if c in ("A", "B") else None
            if tag is None:
                continue
            counts[tag] += 1
            if r.get("clinical"):
                clin[tag] += 1
                wins.append((tag, it["consult"], it["lo"], r.get("why", "")))
    total = sum(counts.values())
    print(f"{key}: {total} judged: closer-to-reference {tag_a} {counts[tag_a]}, {tag_b} {counts[tag_b]}, same {counts['same']}; "
          f"clinical {tag_a} {clin[tag_a]}, {tag_b} {clin[tag_b]}")
    for tag, consult, lo, why in wins:
        print(f"  clinical win {tag}: {consult} {lo:.1f} s: {why}")


if __name__ == "__main__":
    cmd, ta, tb = sys.argv[1:4]
    if cmd == "pack":
        pack(ta, tb, int(sys.argv[4]) if len(sys.argv) > 4 else 35)
    else:
        merge(ta, tb)
