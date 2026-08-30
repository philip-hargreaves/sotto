# Summarises runs.jsonl + events.jsonl from perf_loop.py into report.md
import collections
import json
import os
import statistics

HERE = os.environ.get("PERF_DIR", os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "build", "perf-loop"))


def load(name):
    path = os.path.join(HERE, name)
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as f:
        return [json.loads(line) for line in f if line.strip()]


def pct(values, p):
    values = [v for v in values if v is not None]
    if not values:
        return None
    values.sort()
    k = (len(values) - 1) * p
    lo, hi = int(k), min(int(k) + 1, len(values) - 1)
    return values[lo] + (values[hi] - values[lo]) * (k - lo)


def fmt(v, digits=1):
    return "-" if v is None else f"{v:.{digits}f}"


def stats(values):
    values = [v for v in values if v is not None]
    if not values:
        return "-"
    return f"{fmt(statistics.median(values))} / {fmt(pct(values, 0.95))} / {fmt(max(values))}"


def main():
    runs = load("runs.jsonl")
    events = load("events.jsonl")
    out = []
    w = out.append
    ok = [r for r in runs if r["outcome"] == "ok"]
    deaths = [e for e in events if e["kind"] == "engine_died"]
    starts = [e for e in events if e["kind"] == "engine_started"]
    started = next((e["t"] for e in events if e["kind"] == "loop_started"), "?")
    finished = next((e for e in events if e["kind"] == "loop_finished"), None)

    w("# 1x performance loop\n")
    w(f"Started {started[:16]}Z, "
      + (f"finished after {finished['cycles']} cycles" if finished else "still running")
      + f". {len(runs)} runs, {len(ok)} clean, {len(starts)} engine launches, "
      f"**{len(deaths)} engine deaths**.\n")

    # Outcomes
    counts = collections.Counter(r["outcome"] for r in runs)
    w("## Outcomes\n")
    w("| outcome | runs |\n|---|---|")
    for k, v in counts.most_common():
        w(f"| {k} | {v} |")
    w("")

    # Per track
    w("## Per track (median / p95 / max, seconds)\n")
    w("| track | audio | n | finalise | note first token | note done | sheet done | stop to all done | live lag med | live lag p95 | turns |")
    w("|---|---|---|---|---|---|---|---|---|---|---|")
    by_track = collections.defaultdict(list)
    for r in ok:
        by_track[r["track"]].append(r)
    for track, rs in sorted(by_track.items(), key=lambda kv: kv[1][0]["audio_s"]):
        w(f"| {track} | {rs[0]['audio_s'] // 60} min | {len(rs)} "
          f"| {stats([r.get('finalise_s') for r in rs])} "
          f"| {stats([r.get('note_first_token_s') for r in rs])} "
          f"| {stats([r.get('note_done_s') for r in rs])} "
          f"| {stats([r.get('patient_done_s') for r in rs])} "
          f"| {stats([r.get('stop_to_all_done_s') for r in rs])} "
          f"| {stats([r.get('lag_median_s') for r in rs])} "
          f"| {stats([r.get('lag_p95_s') for r in rs])} "
          f"| {stats([r.get('turns_live') for r in rs])} |")
    w("")

    # Finalise stages
    w("## Finalise stage breakdown (engine's own clock, median seconds since stop)\n")
    stage_names = []
    for r in ok:
        for name in r.get("finalise_stages", {}):
            if name not in stage_names:
                stage_names.append(name)
    w("| track | " + " | ".join(stage_names) + " |")
    w("|---|" + "---|" * len(stage_names))
    for track, rs in sorted(by_track.items(), key=lambda kv: kv[1][0]["audio_s"]):
        cells = []
        for name in stage_names:
            cells.append(fmt(statistics.median([r["finalise_stages"][name] for r in rs
                                                if name in r.get("finalise_stages", {})]) if any(
                name in r.get("finalise_stages", {}) for r in rs) else None))
        w(f"| {track} | " + " | ".join(cells) + " |")
    w("")

    # Engine-side metrics
    w("## Engine metrics per run (median)\n")
    w("| track | ASR realtime factor | lost frames | diar ticks | turns | clusters |")
    w("|---|---|---|---|---|---|")
    for track, rs in sorted(by_track.items(), key=lambda kv: kv[1][0]["audio_s"]):
        ms = [r["metrics"] for r in rs if r.get("metrics")]
        if not ms:
            continue
        med = lambda k: fmt(statistics.median([m[k] for m in ms if m.get(k) is not None])) if any(m.get(k) is not None for m in ms) else "-"
        w(f"| {track} | {med('asrRealtimeFactor')} | {med('lostFrames')} | {med('diarTicks')} | {med('turns')} | {med('clusters')} |")
    w("")

    # Memory
    w("## Engine memory (working set MB after each run, by cycle)\n")
    by_cycle = collections.defaultdict(list)
    for r in runs:
        if r.get("mem_after"):
            by_cycle[r["cycle"]].append((r["run"], r["track"][:4], r["mem_after"]["ws_mb"], r["mem_after"]["private_mb"]))
    w("| cycle | runs (track: ws / private) |")
    w("|---|---|")
    for cycle, items in sorted(by_cycle.items()):
        w(f"| {cycle} | " + ", ".join(f"{t}: {ws}/{pr}" for _, t, ws, pr in items) + " |")
    w("")
    sysmem = [r["sys_after"]["avail_commit_mb"] for r in runs if r.get("sys_after")]
    if sysmem:
        w(f"System commit available: first {sysmem[0]} MB, last {sysmem[-1]} MB, min {min(sysmem)} MB.\n")

    # Cold starts
    w("## Engine launches\n")
    w("| # | echo (s) | ready (s) | ws at ready (MB) |")
    w("|---|---|---|---|")
    for e in starts:
        w(f"| {e['index']} | {fmt(e.get('echo_s'), 2)} | {fmt(e.get('ready_s'))} | {(e.get('mem') or {}).get('ws_mb', '-')} |")
    w("")

    # Deaths and failures
    w("## Engine deaths\n")
    if not deaths:
        w("None.\n")
    for e in deaths:
        w(f"- run {e['run']} ({e['track']}), engine {e['index']}, exit code {e['exit_code']}, "
          f"uptime {e['uptime_s']} s")
        for line in e.get("log_tail", [])[-6:]:
            w(f"  - `{line}`")
    w("")
    failures = [r for r in runs if r["outcome"] != "ok"]
    w("## Failed runs\n")
    if not failures:
        w("None.\n")
    for r in failures:
        w(f"- run {r['run']} {r['track']}: **{r['outcome']}** {r.get('error') or ''} "
          f"{('note: ' + r['note_failed']) if r.get('note_failed') else ''}"
          f"{('sheet: ' + r['patient_failed']) if r.get('patient_failed') else ''}")
    w("")

    with open(os.path.join(HERE, "report.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(out))
    print("\n".join(out))


if __name__ == "__main__":
    main()
