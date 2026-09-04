# Negation audit: the raw screen (score_gate.py) lists every negation token lost, added or
# changed; most are repeats or backchannels. A blinded reader classifies each in context so
# the write-up can say how many change clinical meaning. Packs per arm; merge reads the JSON back.
#   python tools/eval/negation_audit.py pack <arm> [per_pack=40]
#   python tools/eval/negation_audit.py merge <arm>
import glob
import json
import os
import re
import sys

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build", "perf-loop")
OUT = os.path.join(HERE, "negation-audit")

ENTRY = re.compile(r"^(\S+) \[(\w+)\] (\w+): ref=(\S+) hyp=(\S+)\n\s*REF: (.*)\n\s*HYP: (.*)", re.M)

INSTRUCTIONS = """You are auditing negation differences between a reference transcript of a UK primary-care
consultation and an automatic transcript. Each item shows the reference context (REF, the
negation token in [brackets]) and the automatic transcript's context (HYP). Classify each item
by whether the automatic transcript changes the CLINICAL MEANING a reader would take:

  flip        the meaning is reversed or a denial/affirmation is misstated
              (e.g. "no vomiting" -> "vomiting"; "no" answer rendered as "yes"; "don't" -> "do")
  lost_info   a negation is missing and a reader could not recover it (e.g. the whole
              denial is absent), meaning is weakened but not reversed
  redundant   the negation is dropped or changed but the surrounding words still carry the
              same meaning (e.g. "no no I haven't" -> "no I haven't"; "not really" kept)
  backchannel a bare "no"/"nope" acknowledgement or filler, no clinical content
  noise       ASR substitution with no effect on meaning (e.g. "no" -> "now" inside a phrase
              whose sense is unchanged), or a reference tag/inaudible marker

Write ONLY a JSON array of objects {"id": <id>, "class": <one of the five>, "why": <ten words>}
to the results path given, in item order. Do not read any other files."""


def pack(arm, per_pack):
    txt = open(os.path.join(HERE, "negation-diffs.txt"), encoding="utf-8", errors="replace").read()
    items = [m for m in ENTRY.finditer(txt) if m.group(2) == arm]
    os.makedirs(OUT, exist_ok=True)
    n = 0
    for start in range(0, len(items), per_pack):
        n += 1
        lines = [INSTRUCTIONS, "", f"ARM CODE: {arm}   PACK {n}", ""]
        for k, m in enumerate(items[start:start + per_pack], start + 1):
            consult, _, kind, ref, hyp, rctx, hctx = m.groups()
            lines.append(f"[{k}] {kind}: ref={ref} hyp={hyp}")
            lines.append(f"    REF: {rctx}")
            lines.append(f"    HYP: {hctx}")
            lines.append("")
        open(os.path.join(OUT, f"{arm}-pack{n:02d}.txt"), "w", encoding="utf-8", newline="\n").write("\n".join(lines))
    print(f"{arm}: {len(items)} items in {n} packs -> {OUT}")


def merge(arm):
    counts = {}
    total = 0
    for f in sorted(glob.glob(os.path.join(OUT, f"{arm}-pack*-result.json"))):
        for r in json.load(open(f, encoding="utf-8")):
            counts[r["class"]] = counts.get(r["class"], 0) + 1
            total += 1
    print(f"{arm}: {total} audited: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
    flips = []
    for f in sorted(glob.glob(os.path.join(OUT, f"{arm}-pack*-result.json"))):
        for r in json.load(open(f, encoding="utf-8")):
            if r["class"] == "flip":
                flips.append(r)
    for r in flips:
        print(f"  flip #{r['id']}: {r.get('why', '')}")


if __name__ == "__main__":
    cmd, arm = sys.argv[1:3]
    if cmd == "pack":
        pack(arm, int(sys.argv[3]) if len(sys.argv) > 3 else 40)
    else:
        merge(arm)
