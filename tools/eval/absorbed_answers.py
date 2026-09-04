# Absorbed answers: reference turns of at most N words heard under the OTHER speaker,
# and missing answers: words absent near their time. This is the error class the blinded
# attribution judge sees and word attribution does not. Per arm, plus the paired difference.
#   python tools/eval/absorbed_answers.py evalS16 evalC16 [max_words=12]
# ABSORBED_SUBSTANTIVE=1: only answers a professional transcriber would keep (a content
# word, or a bare yes/no replying to the other speaker's question).
import glob
import json
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "perf-loop"))
import score_gate  # noqa: E402

TAGS = sys.argv[1:3] if len(sys.argv) >= 3 else ["evalS16", "evalC16"]
MAX_WORDS = int(sys.argv[3]) if len(sys.argv) > 3 else 12
MIN_OVERLAP = 0.5
SUBSTANTIVE = os.environ.get("ABSORBED_SUBSTANTIVE", "") == "1"
FILLER = {"ok", "okay", "yeah", "yep", "yup", "mm", "mhm", "mmhmm", "mmm", "hmm", "hm", "um",
          "uh", "er", "erm", "huh", "sure", "right", "alright", "all", "fine", "good", "great",
          "thank", "thanks", "thankyou", "you", "bye", "goodbye", "hello", "hi", "hey", "cheers",
          "brilliant", "lovely", "perfect", "cool", "fab", "super", "so", "and", "oh", "ohh",
          "ah", "aha", "yes", "no", "nope", "not", "really", "k", "o", "the", "a", "i", "it",
          "that", "is", "s", "well", "just", "like", "morning", "afternoon", "evening", "how",
          "are", "take", "care", "then", "see", "sorry", "pardon", "now", "please", "welcome",
          "too", "nice", "meet", "pleasure", "later", "ta", "of", "course", "absolutely",
          "definitely", "exactly", "indeed", "sounds", "there", "we", "this", "go", "on", "do",
          "does", "did", "will", "have", "had", "got", "to", "me", "my", "your", "know", "mean",
          "think", "very", "much", "bit", "little", "ill", "im", "youre", "its", "thats", "in",
          "at", "with", "for", "be", "been", "was", "were", "am", "yours", "one", "two", "sec",
          "second", "moment", "wait", "hang", "hold", "let", "us", "ready", "here", "come",
          "coming", "fantastic", "excellent", "wonderful", "amazing", "no", "worries", "problem",
          "understood", "understand", "gotcha", "got"}
YES_NO = {"yes", "yeah", "yep", "yup", "no", "nope"}


# A content word makes an utterance substantive; a filler-only utterance is
# substantive only as the direct reply to the other speaker's question
def substantive(words, prev_other_text):
    if any(w not in FILLER for w in words):
        return True
    return prev_other_text.strip().endswith("?")


def load(tag, consult):
    path = os.path.join(score_gate.HERE, "transcripts", f"{tag}-{consult}_mixed.json")
    if not os.path.exists(path):
        return None
    turns = json.load(open(path, encoding="utf-8"))["turns"]
    return [(t["firstFrame"] / 16000.0, (t["firstFrame"] + t["frameCount"]) / 16000.0, t["speaker"],
             " ".join(score_gate.normalise(t["text"]))) for t in turns if t["text"].strip()]


def classify(ref_ivs, hyp):
    c = Counter()
    absorbed = []
    ordered = sorted(ref_ivs, key=lambda iv: iv[0])
    last_text = {"doctor": "", "patient": ""}
    for start, end, spk, text in ordered:
        other = "patient" if spk == "doctor" else "doctor"
        prev_other = last_text[other]
        last_text[spk] = text
        words = score_gate.normalise(text)
        if not words or len(words) > MAX_WORDS:
            continue
        if SUBSTANTIVE and not substantive(words, prev_other):
            continue
        c["answers"] += 1
        dur = max(1e-6, end - start)
        key = " ".join(words[:3]) if len(words) >= 2 else words[0]
        near = [(s, e, hs, hw) for s, e, hs, hw in hyp if e >= start - 3 and s <= start + 6]
        # Kept means the words are there under the right speaker, not merely that a
        # same-speaker turn covers the time
        covering = [hw for s, e, hs, hw in hyp if hs == spk and min(end, e) > max(start, s)]
        covered = sum(max(0.0, min(end, e) - max(start, s)) for s, e, hs, _ in hyp if hs == spk)
        heard = set(w for hw in covering for w in hw.split())
        if covered / dur >= MIN_OVERLAP and any(w in heard for w in words):
            c["kept"] += 1
            continue
        if any(key in hw and hs != spk for s, e, hs, hw in near):
            c["absorbed"] += 1
            absorbed.append((round(start, 1), spk, text))
        elif any(key in hw for s, e, hs, hw in near):
            c["present_low_overlap"] += 1
        else:
            c["dropped"] += 1
    return c, absorbed


def main():
    consults = sorted(os.path.basename(p)[:-len("_doctor.TextGrid")]
                      for p in glob.glob(os.path.join(score_gate.REFS, "*_doctor.TextGrid")))
    tot = {t: Counter() for t in TAGS}
    per = []
    for consult in consults:
        arms = {t: load(t, consult) for t in TAGS}
        if any(v is None for v in arms.values()):
            continue
        ref_ivs, _ = score_gate.reference(consult)
        row = {"consult": consult}
        for t in TAGS:
            c, absorbed = classify(ref_ivs, arms[t])
            tot[t] += c
            row[t] = {"absorbed": c["absorbed"], "dropped": c["dropped"], "list": absorbed}
        per.append(row)
    out = os.path.join(score_gate.HERE, f"absorbed-{TAGS[0]}-vs-{TAGS[1]}.json")
    json.dump(per, open(out, "w", encoding="utf-8"), indent=1)
    print(f"{len(per)} consults, reference answers <= {MAX_WORDS} words\n")
    print("| arm | answers | kept | absorbed (other speaker) | dropped | present, low overlap |")
    print("|---|---|---|---|---|---|")
    for t in TAGS:
        c = tot[t]
        print(f"| {t} | {c['answers']} | {c['kept']} | **{c['absorbed']}** | {c['dropped']} | {c['present_low_overlap']} |")
    a, b = TAGS
    d = [r[b]["absorbed"] - r[a]["absorbed"] for r in per]
    print(f"\npaired absorbed {b} - {a}: {b} fewer in {sum(1 for x in d if x < 0)}, more in {sum(1 for x in d if x > 0)}, equal in {sum(1 for x in d if x == 0)}")
    print(f"per-consult JSON: {out}")


if __name__ == "__main__":
    main()
