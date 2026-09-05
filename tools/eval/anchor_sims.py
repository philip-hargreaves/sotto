# Read the "roles anchor sims" lines a sweep's engines logged at every seal and put them
# beside the consultation they belong to, so the resemblance between each speaker and the
# stored print can be compared between the print's own clinician and the others.
#   python tools/eval/anchor_sims.py <tag> [own-day-prefix]      e.g. ENR16L day1
# Engines run several consults each; the sweep log gives the order per engine and the
# engine's seals are paired with them in that order.
import os
import re
import sys

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build")
OK = re.compile(r"(\S+_mixed)\.wav\s+ok")
SIMS = re.compile(r"roles anchor sims((?: -?[\d.]+)+|\s+none) margin ([\d.]+) -> doctor (-?\d+)")


def seals(tag):
    """(consult, sims, margin, doctor) per seal, in sweep order."""
    consults_by_engine = {}
    order = []
    engine = None
    log = os.path.join(HERE, "eval", f"sweep-{tag}.log")
    for line in open(log, encoding="utf-8", errors="replace"):
        m = re.search(r"engine (\d+) pid", line)
        if m:
            engine = int(m.group(1))
            consults_by_engine.setdefault(engine, [])
            continue
        m = OK.search(line)
        if m and engine is not None:
            consults_by_engine[engine].append(m.group(1))
            order.append(m.group(1))
    by_consult = {}
    for engine, consults in sorted(consults_by_engine.items()):
        path = os.path.join(HERE, "eval", "logs", tag, f"engine-{engine:03d}.log")
        if not os.path.exists(path):
            print("missing log", path)
            continue
        found = [SIMS.search(l) for l in open(path, encoding="utf-8", errors="replace")]
        found = [m for m in found if m]
        if len(found) != len(consults):
            print(f"engine {engine}: {len(found)} seals for {len(consults)} consults; pairing in order")
        for consult, m in zip(consults, found):
            sims = [] if "none" in m.group(1) else [float(x) for x in m.group(1).split()]
            by_consult[consult] = (consult, sims, float(m.group(2)), int(m.group(3)))
    return [by_consult[c] for c in order if c in by_consult]


def main():
    tag = sys.argv[1]
    own = sys.argv[2] if len(sys.argv) > 2 else None
    rows = seals(tag)
    print(f"{tag}: {len(rows)} seals")
    own_best, foreign_best = [], []
    for consult, sims, margin, doctor in sorted(rows):
        best = max(sims) if sims else float("nan")
        second = sorted(sims)[-2] if len(sims) > 1 else float("nan")
        kind = "own" if own and consult.startswith(own) else "foreign" if own else "-"
        (own_best if kind == "own" else foreign_best).append(best)
        print(f"  {consult:22s} {kind:8s} best {best:.3f} second {second:.3f} "
              f"margin {margin:.3f} doctor {doctor}")
    if not own:
        return

    def desc(v):
        v = sorted(v)
        return (f"n={len(v)} min {v[0]:.3f} median {v[len(v) // 2]:.3f} max {v[-1]:.3f}"
                if v else "n=0")

    print(f"own clinician, best similarity:     {desc(own_best)}")
    print(f"other clinicians, best similarity:  {desc(foreign_best)}")
    if own_best and foreign_best:
        gap = min(own_best) - max(foreign_best)
        verdict = "a floor fits between them" if gap > 0 else "the distributions overlap"
        print(f"gap (min own - max foreign): {gap:+.3f}  -> {verdict}")


if __name__ == "__main__":
    main()
