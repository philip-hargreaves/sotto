"""Stop-to-last-token comparison across note tiers, from tier_campaign.ps1's output.

Reads runs-tier-*.jsonl and logs/tier-*-<track>.log under build/perf-loop and prints one
markdown table per track: the stop path phase by phase, the load that arm paid at engine
start, prefill coverage and decode rate from the engine log.
"""
import glob
import json
import os
import re
import sys

HERE = os.environ.get("PERF_DIR", os.path.join(os.path.dirname(__file__), "..", "..", "build", "perf-loop"))
ARMS = [("tier-4b", "4B"), ("tier-9b", "9B"), ("tier-35b", "35B"), ("tier-35b-pf", "35B + VLM prefill")]


def runs(tag):
    path = os.path.join(HERE, f"runs-{tag}.jsonl")
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as f:
        return [json.loads(line) for line in f if line.strip()]


def log_facts(tag, track):
    path = os.path.join(HERE, "logs", f"{tag}-{track}.log")
    facts = {"load": None, "verify": None, "asr_wait": 0.0, "covered": None, "tok_s": [], "host_first": []}
    if not os.path.exists(path):
        return facts
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.search(r"checked in ([\d.]+) s, loaded in ([\d.]+) s", line)
            if m:
                facts["verify"], facts["load"] = float(m.group(1)), float(m.group(2))
            m = re.search(r"asr waited ([\d.]+) s", line)
            if m:
                facts["asr_wait"] += float(m.group(1))
            m = re.search(r"prefill covered \d+ \((\d+)%\)", line)
            if m:
                facts["covered"] = int(m.group(1))
            m = re.search(r"(\d+) tokens in [\d.]+ s, ([\d.]+) tok/s", line)
            if m:
                facts["tok_s"].append(float(m.group(2)))
            m = re.search(r"note-host: first token in ([\d.]+) s", line)
            if m:
                facts["host_first"].append(float(m.group(1)))
    return facts


def fmt(value, unit="", digits=1):
    if value is None:
        return "-"
    if isinstance(value, float):
        return f"{value:.{digits}f}{unit}"
    return f"{value}{unit}"


def main():
    tracks = sys.argv[1:] or ["c02m", "c05m", "c10m"]
    by_arm = {tag: {r["track"].split("_")[0]: r for r in runs(tag)} for tag, _ in ARMS}
    for track in tracks:
        print(f"\n### {track}\n")
        print("| | " + " | ".join(name for _, name in ARMS) + " |")
        print("|---|" + "---|" * len(ARMS))
        rows = []
        cells = {tag: (by_arm[tag].get(track), log_facts(tag, track)) for tag, _ in ARMS}

        def row(label, fn):
            rows.append("| " + label + " | " + " | ".join(fn(*cells[tag]) for tag, _ in ARMS) + " |")

        row("outcome", lambda r, f: r["outcome"] if r else "-")
        row("model load at engine start (presence/size check)", lambda r, f: f"{fmt(f['load'], ' s')} ({fmt(f['verify'], ' s')})")
        row("Whisper waited for the lease, total", lambda r, f: fmt(f["asr_wait"], " s"))
        row("stop reply (transcript sealed)", lambda r, f: fmt(r.get("finalise_s"), " s") if r else "-")
        row("prefill covered at stop", lambda r, f: fmt(f["covered"], " %"))
        row("stop → note first token", lambda r, f: fmt(r.get("note_first_token_s"), " s") if r else "-")
        row("stop → note done", lambda r, f: fmt(r.get("note_done_s"), " s") if r else "-")
        row("note length", lambda r, f: fmt(r.get("note_chars"), " chars") if r else "-")
        row("note decode rate", lambda r, f: fmt(f["tok_s"][0], " tok/s") if f["tok_s"] else "-")
        row("note done → sheet first token", lambda r, f: fmt(r.get("patient_first_token_s"), " s") if r else "-")
        row("note done → sheet done", lambda r, f: fmt(r.get("patient_done_s"), " s") if r else "-")
        row("sheet decode rate", lambda r, f: fmt(f["tok_s"][1], " tok/s") if len(f["tok_s"]) > 1 else "-")
        row("stop → everything on screen", lambda r, f: fmt(r.get("stop_to_all_done_s"), " s") if r else "-")
        row("engine deaths", lambda r, f: "0" if r and r.get("outcome") == "ok" else (r.get("error") if r else "-"))
        print("\n".join(rows))


if __name__ == "__main__":
    main()
