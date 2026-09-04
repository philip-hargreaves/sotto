# Scores the pause-gate sweep transcripts against the PriMock human transcripts:
# word error rate, negation mismatches (every not/no/n't/never lost, added or
# flipped), speaker attribution by time overlap, and a file of every negation
# difference with context, for reading.
import glob
import json
import os
import re
import sys

HERE = os.environ.get("PERF_DIR", os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "build", "perf-loop"))
REFS = r"C:\dev\intelliscribe\data\primock57\transcripts"
TAGS = sys.argv[1:] or ["sweep20", "sweep4"]

NEGATIONS = {"not", "no", "never", "none", "nothing", "neither", "nor", "without", "cannot",
             "dont", "doesnt", "didnt", "isnt", "arent", "wasnt", "werent", "havent", "hasnt",
             "hadnt", "wont", "wouldnt", "cant", "couldnt", "shouldnt", "mustnt", "nope"}


def normalise(text):
    text = text.lower().replace("’", "'")
    text = re.sub(r"<[^>]*>", " ", text)  # <UNIN/>, <INAUD/> markers
    text = re.sub(r"[^a-z0-9' ]+", " ", text)
    words = []
    for w in text.split():
        w = w.replace("'", "")
        if w in ("um", "uh", "erm", "er", "hmm", "mm", "mhm"):
            continue
        words.append(w)
    return words


def parse_textgrid(path, speaker):
    intervals = []
    xmin = None
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        m = re.match(r"xmin = ([\d.]+)", line)
        if m:
            xmin = float(m.group(1))
        m = re.match(r"xmax = ([\d.]+)", line)
        if m:
            xmax = float(m.group(1))
        m = re.match(r'text = "(.*)"', line)
        if m and m.group(1).strip():
            intervals.append((xmin, xmax, speaker, m.group(1)))
    return intervals


def reference(consult):
    ivs = parse_textgrid(os.path.join(REFS, f"{consult}_doctor.TextGrid"), "doctor")
    ivs += parse_textgrid(os.path.join(REFS, f"{consult}_patient.TextGrid"), "patient")
    ivs.sort()
    words = []
    for start, end, speaker, text in ivs:
        for w in normalise(text):
            words.append((w, speaker))
    return ivs, words


# NEG_SUBSTANTIVE=1: count a reference negation only when its utterance holds a content
# word (or is a filler-only reply to the other speaker's question) and it is not an
# immediate repeat ("no no no" counts once). Added negations always count.
FILLER_NEG = {"ok", "okay", "yeah", "yep", "yup", "mm", "mhm", "hmm", "um", "uh", "er", "erm",
              "sure", "right", "alright", "all", "fine", "good", "thank", "thanks", "you", "bye",
              "hello", "hi", "oh", "ohh", "ah", "yes", "no", "nope", "not", "really", "so", "and",
              "the", "a", "i", "it", "that", "is", "well", "just", "like", "nothing", "none",
              "never", "dont", "cant", "havent", "didnt", "im", "its", "thats", "there", "at",
              "all", "sorry", "please"}


def substantive_flags(ivs):
    # one flag per reference word, in reference() order
    flags = []
    last_text = {"doctor": "", "patient": ""}
    for start, end, speaker, text in ivs:
        other = "patient" if speaker == "doctor" else "doctor"
        prev_other = last_text[other]
        last_text[speaker] = text
        ws = normalise(text)
        content = any(w not in FILLER_NEG for w in ws)
        answer = prev_other.strip().endswith("?")
        keep = content or answer
        prev = None
        for w in ws:
            flags.append(keep and w != prev)
            prev = w
    return flags


def align(ref, hyp):
    # Levenshtein with backtrace; returns ops as (op, ref_i, hyp_j)
    n, m = len(ref), len(hyp)
    d = [[0] * (m + 1) for _ in range(n + 1)]
    for i in range(n + 1):
        d[i][0] = i
    for j in range(m + 1):
        d[0][j] = j
    for i in range(1, n + 1):
        ri = ref[i - 1]
        row, prev = d[i], d[i - 1]
        for j in range(1, m + 1):
            cost = 0 if ri == hyp[j - 1] else 1
            row[j] = min(prev[j] + 1, row[j - 1] + 1, prev[j - 1] + cost)
    ops = []
    i, j = n, m
    while i > 0 or j > 0:
        if i > 0 and j > 0 and d[i][j] == d[i - 1][j - 1] + (0 if ref[i - 1] == hyp[j - 1] else 1):
            ops.append(("ok" if ref[i - 1] == hyp[j - 1] else "sub", i - 1, j - 1))
            i, j = i - 1, j - 1
        elif i > 0 and d[i][j] == d[i - 1][j] + 1:
            ops.append(("del", i - 1, None))
            i -= 1
        else:
            ops.append(("ins", None, j - 1))
            j -= 1
    ops.reverse()
    return d[n][m], ops


def context(words, k, span=6):
    lo, hi = max(0, k - span), min(len(words), k + span + 1)
    return " ".join(words[lo:k] + ["[" + words[k] + "]"] + words[k + 1:hi])


def score(tag, consult, ref_ivs, ref_words):
    path = os.path.join(HERE, "transcripts", f"{tag}-{consult}_mixed.json")
    if not os.path.exists(path):
        return None
    data = json.load(open(path, encoding="utf-8"))
    turns = data["turns"]
    hyp_words, hyp_speaker = [], []
    for t in turns:
        ws = normalise(t["text"])
        hyp_words += ws
        hyp_speaker += [t["speaker"]] * len(ws)
    ref_plain = [w for w, _ in ref_words]
    errors, ops = align(ref_plain, hyp_words)
    substantive = os.environ.get("NEG_SUBSTANTIVE", "") == "1"
    flags = substantive_flags(ref_ivs) if substantive else None
    neg = []
    for op, i, j in ops:
        rw = ref_plain[i] if i is not None else None
        hw = hyp_words[j] if j is not None else None
        if op == "ok":
            continue
        if substantive and i is not None and not flags[i]:
            continue
        if (rw in NEGATIONS) or (hw in NEGATIONS):
            kind = {"del": "lost", "ins": "added", "sub": "changed"}[op]
            neg.append({"kind": kind, "ref": rw, "hyp": hw,
                        "ref_ctx": context(ref_plain, i) if i is not None else "",
                        "hyp_ctx": context(hyp_words, j) if j is not None else ""})
    # Attribution: each hypothesis turn's words scored against the reference
    # speaker holding the majority of its time span
    attributed = total = 0
    for t in turns:
        ws = len(normalise(t["text"]))
        if ws == 0 or t["speaker"] not in ("doctor", "patient"):
            continue
        start, end = t["firstFrame"] / 16000, (t["firstFrame"] + t["frameCount"]) / 16000
        share = {"doctor": 0.0, "patient": 0.0}
        for s, e, spk, _ in ref_ivs:
            share[spk] += max(0.0, min(end, e) - max(start, s))
        majority = max(share, key=share.get)
        total += ws
        if share[majority] > 0 and majority == t["speaker"]:
            attributed += ws
    return {"wer": errors / max(1, len(ref_plain)), "errors": errors, "ref_words": len(ref_plain),
            "hyp_words": len(hyp_words), "turns": len(turns), "neg": neg,
            "attr": attributed / max(1, total)}


def main():
    consults = sorted(os.path.basename(p)[:-len("_doctor.TextGrid")]
                      for p in glob.glob(os.path.join(REFS, "*_doctor.TextGrid")))
    rows = []
    neg_out = []
    for consult in consults:
        ref_ivs, ref_words = reference(consult)
        results = {tag: score(tag, consult, ref_ivs, ref_words) for tag in TAGS}
        if any(r is None for r in results.values()):
            continue
        rows.append((consult, results))
        for tag in TAGS:
            for n in results[tag]["neg"]:
                neg_out.append(f"{consult} [{tag}] {n['kind']}: ref={n['ref']} hyp={n['hyp']}\n"
                               f"    REF: {n['ref_ctx']}\n    HYP: {n['hyp_ctx']}\n")
    if not rows:
        print("no scored consults yet")
        return
    print(f"{len(rows)} consults scored: " + " vs ".join(TAGS))
    print(f"{'consult':<22}" + "".join(f"{t + ' WER':>12}{t + ' neg':>10}{t + ' attr':>11}" for t in TAGS))
    tot = {t: {"err": 0, "ref": 0, "neg": 0, "attr_w": 0.0, "n": 0} for t in TAGS}
    better = worse = same = 0
    for consult, results in rows:
        line = f"{consult:<22}"
        for t in TAGS:
            r = results[t]
            line += f"{r['wer'] * 100:>11.2f}%{len(r['neg']):>10}{r['attr'] * 100:>10.1f}%"
            tot[t]["err"] += r["errors"]
            tot[t]["ref"] += r["ref_words"]
            tot[t]["neg"] += len(r["neg"])
            tot[t]["attr_w"] += r["attr"]
            tot[t]["n"] += 1
        if len(TAGS) == 2:
            a, b = results[TAGS[0]]["errors"], results[TAGS[1]]["errors"]
            better += b < a
            worse += b > a
            same += a == b
        print(line)
    print("-" * 22 + "".join(f"{'':>33}" for _ in TAGS))
    line = f"{'TOTAL':<22}"
    for t in TAGS:
        line += (f"{tot[t]['err'] / tot[t]['ref'] * 100:>11.2f}%{tot[t]['neg']:>10}"
                 f"{tot[t]['attr_w'] / tot[t]['n'] * 100:>10.1f}%")
    print(line)
    if len(TAGS) == 2:
        print(f"\nper consult, {TAGS[1]} vs {TAGS[0]}: fewer word errors in {better}, more in {worse}, equal in {same}")
    neg_path = os.path.join(HERE, "negation-diffs.txt")
    with open(neg_path, "w", encoding="utf-8") as f:
        f.write("\n".join(neg_out))
    print(f"\n{len(neg_out)} negation differences written to {neg_path}")


if __name__ == "__main__":
    main()
