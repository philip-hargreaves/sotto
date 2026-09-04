# Calibrate the re-split margin (core/resplit.hpp) from a dry-run sweep: each logged
# edge-chunk candidate ("resplit-candidate <a>-<b> s cluster <c> own <x> other <y> ...") is
# labelled against the reference TextGrids and the rule "other - own >= margin" is scored
# over a range of margins, with cross-validation.
#   python tools/eval/resplit_calibrate.py <engine log> <sweep log> <tag>
# The engine log is one process for the whole sweep; consults are split at the
# "session audio" lines and matched in order with the sweep log's "ok" lines.
import json
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "perf-loop"))
import score_gate  # noqa: E402

CAND = re.compile(r"resplit-candidate ([\d.]+)-([\d.]+) s cluster (\d+) own ([-\d.]+) other ([-\d.]+) (moves|stays) '(.*)'$")


def consults_in_order(sweep_log):
    out = []
    for line in open(sweep_log, encoding="utf-8", errors="replace"):
        m = re.search(r"(\S+)_mixed\.wav ok", line)
        if m:
            out.append(m.group(1))
    return out


def candidates_by_consult(engine_log, order):
    blocks = [[]]
    for line in open(engine_log, encoding="utf-8", errors="replace"):
        if "session audio" in line and "capture ticks" in line:
            blocks.append([])
        m = CAND.search(line)
        if m:
            blocks[-1].append(m)
    # the first block precedes the first finalise; candidates arrive in the finalise block
    blocks = [b for b in blocks[1:]]
    return dict(zip(order, blocks))


def reference_speaker(ref_ivs, a, b):
    # majority speaker by overlap time in [a, b)
    time = {"doctor": 0.0, "patient": 0.0}
    for start, end, spk, text in ref_ivs:
        if not text.strip():
            continue
        ov = min(end, b) - max(start, a)
        if ov > 0:
            time[spk] += ov
    if time["doctor"] == 0 and time["patient"] == 0:
        return None
    return "doctor" if time["doctor"] >= time["patient"] else "patient"


def own_role(turns, a, b):
    best, best_ov = None, 0.0
    for t in turns:
        s = t["firstFrame"] / 16000.0
        e = (t["firstFrame"] + t["frameCount"]) / 16000.0
        ov = min(e, b) - max(s, a)
        if ov > best_ov:
            best, best_ov = t["speaker"], ov
    return best


def main():
    engine_log, sweep_log, tag = sys.argv[1:4]
    order = consults_in_order(sweep_log)
    cands = candidates_by_consult(engine_log, order)
    rows = []
    for consult, ms in cands.items():
        path = os.path.join(score_gate.HERE, "transcripts", f"{tag}-{consult}_mixed.json")
        if not os.path.exists(path):
            continue
        turns = json.load(open(path, encoding="utf-8"))["turns"]
        ref, _ = score_gate.reference(consult)
        for m in ms:
            a, b = float(m.group(1)), float(m.group(2))
            own, other = float(m.group(4)), float(m.group(5))
            truth = reference_speaker(ref, a, b)
            mine = own_role(turns, a, b)
            if truth is None or mine is None or mine not in ("doctor", "patient"):
                continue
            rows.append({"consult": consult, "a": a, "b": b, "dur": b - a, "margin": other - own,
                         "should_move": truth != mine, "text": m.group(7)})
    print(f"{len(rows)} candidates on {len(cands)} consults; {sum(r['should_move'] for r in rows)} should move")
    print("| margin | flips | correct | wrong | net | wrong under 1 s |")
    print("|---|---|---|---|---|---|")
    for margin in [0.0, 0.05, 0.10, 0.15, 0.20, 0.25, 0.30, 0.35, 0.40]:
        flips = [r for r in rows if r["margin"] >= margin]
        correct = sum(1 for r in flips if r["should_move"])
        wrong = len(flips) - correct
        short_wrong = sum(1 for r in flips if not r["should_move"] and r["dur"] < 1.0)
        print(f"| {margin:.2f} | {len(flips)} | {correct} | {wrong} | {correct - wrong:+d} | {short_wrong} |")
    # Cross-validation: choose the margin on the other consults (safe side: fewest
    # wrong moves, then most correct), apply it to the held-out ones, sum over folds
    grid = [x / 100.0 for x in range(0, 61, 5)]

    def choose(train):
        best = None
        for m in grid:
            flips = [r for r in train if r["margin"] >= m]
            wrong = sum(1 for r in flips if not r["should_move"])
            correct = len(flips) - wrong
            key = (wrong, -correct, m)
            if best is None or key < best[0]:
                best = (key, m)
        return best[1]

    def held_out(rows_train, rows_test):
        m = choose(rows_train)
        flips = [r for r in rows_test if r["margin"] >= m]
        wrong = sum(1 for r in flips if not r["should_move"])
        return m, len(flips) - wrong, wrong

    consults = sorted(set(r["consult"] for r in rows))
    print()
    for name, folds in [("leave-one-consult-out", [[c] for c in consults]),
                        ("5-fold by consult", [consults[i::5] for i in range(5)]),
                        ("by day (clinician)", [[c for c in consults if c.startswith(d)] for d in sorted(set(c[:4] for c in consults))])]:
        chosen, correct, wrong = [], 0, 0
        for fold in folds:
            if not fold:
                continue
            train = [r for r in rows if r["consult"] not in fold]
            test = [r for r in rows if r["consult"] in fold]
            m, c, w = held_out(train, test)
            chosen.append(m)
            correct += c
            wrong += w
        chosen.sort()
        print(f"{name}: held-out correct {correct}, wrong {wrong}, net {correct - wrong:+d}; "
              f"chosen margins min {chosen[0]:.2f} median {chosen[len(chosen) // 2]:.2f} max {chosen[-1]:.2f}")
    out = os.path.join(score_gate.HERE, f"resplit-candidates-{tag}.json")
    json.dump(rows, open(out, "w", encoding="utf-8"), indent=1)
    print(f"candidates written: {out}")


if __name__ == "__main__":
    main()
