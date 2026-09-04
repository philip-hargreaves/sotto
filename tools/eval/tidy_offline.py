# Offline port of core/tidy_transcript.hpp so a tidied copy of an existing sweep
# can be scored against its untidied self: same decode, same cuts, only the tidy differs.
# The C++ is the source of truth; this mirrors its three rules and is checked against the
# engine's own tidied output where both exist.
#   python tools/eval/tidy_offline.py <in_tag> <out_tag>      e.g. F1S16 F1S16T
import glob
import json
import os
import re
import sys

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build", "perf-loop", "transcripts")
MERGE_GAP = 16000
DISFLUENCY = {"um", "uh", "er", "erm", "hm", "hmm", "mm", "mmm", "mhm", "ah", "eh", "huh"}
FUNCTION = {"and", "so", "but", "or", "the", "a", "an", "to", "of", "in", "i", "it"}


def norm(w):
    return "".join(c.lower() for c in w if c.isalnum() or c == "'")


def no_content(text):
    words = 0
    function_words = 0
    for raw in text.split():
        w = norm(raw)
        if not w:
            continue
        words += 1
        if w in DISFLUENCY:
            continue
        if w in FUNCTION:
            function_words += 1
            continue
        return False
    return words > 0 and function_words <= 2


def ends_sentence(text):
    for c in reversed(text):
        if c.isspace() or c in "\"')":
            continue
        return c in ".?!"
    return False


def capitalised(text):
    out = list(text)
    for i, c in enumerate(out):
        if c.isalpha():
            out[i] = c.upper()
            break
        if c.isdigit():
            break
    text = "".join(out)
    return re.sub(r"(?<![^\s])i(?=$|\s|'|,|\.)", "I", text)


def terminated(text):
    text = text.rstrip()
    if not text or ends_sentence(text):
        return text
    if text[-1] in ",;:":
        text = text[:-1]
    if text and text[-1].isalnum():
        text += "."
    return text


def tidy(turns):
    merged = []
    for t in turns:
        if not t["text"].strip():
            continue
        t = dict(t)
        if merged and merged[-1]["speaker"] == t["speaker"]:
            prev = merged[-1]
            prev_end = prev["firstFrame"] + prev["frameCount"]
            gap = t["firstFrame"] - prev_end if t["firstFrame"] > prev_end else 0
            if gap <= MERGE_GAP:
                trail_off = prev["text"].endswith("...")
                piece = capitalised(t["text"]) if ends_sentence(prev["text"]) and not trail_off else t["text"]
                prev["text"] = prev["text"] + " " + piece
                end = t["firstFrame"] + t["frameCount"]
                if end > prev_end:
                    prev["frameCount"] = end - prev["firstFrame"]
                continue
        merged.append(t)
    out = []
    for t in merged:
        if no_content(t["text"]):
            continue
        t["text"] = terminated(capitalised(t["text"]))
        out.append(t)
    return out


def main():
    src, dst = sys.argv[1:3]
    n = 0
    for path in glob.glob(os.path.join(HERE, f"{src}-*_mixed.json")):
        data = json.load(open(path, encoding="utf-8"))
        data["turns"] = tidy(data["turns"])
        out = path.replace(f"{src}-", f"{dst}-", 1)
        json.dump(data, open(out, "w", encoding="utf-8"), indent=1)
        n += 1
    print(f"{n} transcripts tidied: {src} -> {dst}")


if __name__ == "__main__":
    main()
