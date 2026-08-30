# Summarise the EcoQoS A/B/C test: runs-ab.jsonl joined with ab-post.jsonl
import collections, json, os, statistics

HERE = os.environ.get("PERF_DIR", os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "build", "perf-loop"))


def load(name):
    p = os.path.join(HERE, name)
    return [json.loads(l) for l in open(p, encoding="utf-8") if l.strip()] if os.path.exists(p) else []


def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None


def rng(xs):
    xs = [x for x in xs if x is not None]
    return f"{min(xs):.1f}-{max(xs):.1f}" if xs else "-"


def f(x, d=1):
    return "-" if x is None else f"{x:.{d}f}"


runs = load("runs-ab.jsonl")
post = {p["run"]: p for p in load("ab-post.jsonl")}
for r in runs:
    r.update({k: v for k, v in post.get(r["run"], {}).items() if k not in r})

out = []
w = out.append
w(f"# EcoQoS A/B/C - {len(runs)} runs, {sum(r['outcome'] == 'ok' for r in runs)} clean\n")

# Verification that the state was actually applied
w("## Throttling state read back from the processes\n")
w("| condition | n | engine state (control/state) | host state | host found after |")
w("|---|---|---|---|---|")
by_c = collections.defaultdict(list)
for r in runs:
    by_c[r["condition"]].append(r)
for c in ("default", "eco_on", "eco_off"):
    rs = by_c.get(c, [])
    def states(key):
        vals = collections.Counter(
            f"{(r.get(key) or {}).get('control')}/{(r.get(key) or {}).get('state')}" for r in rs)
        return ", ".join(f"{k} x{v}" for k, v in vals.items())
    found = [(r.get("host") or {}).get("found_after_s") for r in rs]
    w(f"| {c} | {len(rs)} | {states('engine_throttle_read')} | {states('host_throttle_read')} | {f(med(found))} s |")
w("")

w("## Per track and condition (median, with range)\n")
w("| track | condition | n | finalise | diarised at | decoded at | note first token | 9B load | CPU MHz during finalise | MHz at start |")
w("|---|---|---|---|---|---|---|---|---|---|")
groups = collections.defaultdict(list)
for r in runs:
    groups[(r["track"], r["condition"])].append(r)
tracks = sorted({r["track"] for r in runs}, key=lambda t: next(r["audio_s"] for r in runs if r["track"] == t))
for t in tracks:
    for c in ("default", "eco_on", "eco_off"):
        rs = [r for r in groups.get((t, c), []) if r["outcome"] == "ok"]
        if not rs:
            continue
        fin = [r["finalise_s"] for r in rs]
        dia = [r["finalise_stages"].get("diarised") for r in rs]
        dec = [r["finalise_stages"].get("turns decoded") for r in rs]
        nft = [r.get("note_first_token_s") for r in rs]
        load = [r.get("note_load_s") for r in rs]
        mhz = [(r.get("stop_probe") or {}).get("perf_pct_mean", (r.get("stop_probe") or {}).get("mean_mhz")) for r in rs]
        mhz0 = [r.get("live_mhz_mean_at_start") for r in rs]
        w(f"| {t[:4]} | {c} | {len(rs)} | {f(med(fin))} ({rng(fin)}) | {f(med(dia))} ({rng(dia)}) | {f(med(dec))} | "
          f"{f(med(nft))} | {f(med(load))} ({rng(load)}) | {f(med(mhz), 0)} | {f(med(mhz0), 0)} |")
w("")

w("## Effect vs default (ratio of medians, pooled over tracks)\n")
w("| metric | eco_on / default | eco_off / default |")
w("|---|---|---|")
def pooled(metric):
    res = {}
    for c in ("default", "eco_on", "eco_off"):
        vals = []
        for t in tracks:
            rs = [r for r in groups.get((t, c), []) if r["outcome"] == "ok"]
            m = med([metric(r) for r in rs])
            d = med([metric(r) for r in groups.get((t, "default"), []) if r["outcome"] == "ok"])
            if m is not None and d:
                vals.append(m / d)
        res[c] = med(vals)
    return res
for name, fn in [("finalise", lambda r: r["finalise_s"]),
                 ("diarisation stage", lambda r: r["finalise_stages"].get("diarised")),
                 ("9B load", lambda r: r.get("note_load_s")),
                 ("note first token", lambda r: r.get("note_first_token_s")),
                 ("CPU MHz during finalise", lambda r: (r.get("stop_probe") or {}).get("perf_pct_mean", (r.get("stop_probe") or {}).get("mean_mhz")))]:
    p = pooled(fn)
    w(f"| {name} | {f(p.get('eco_on'), 2)} | {f(p.get('eco_off'), 2)} |")
w("")
failures = [r for r in runs if r["outcome"] != "ok"]
w(f"Failed runs: {len(failures)}" + ("".join(f"\n- run {r['run']} {r['condition']} {r['track']}: {r['outcome']} {r.get('error')}" for r in failures)))
open(os.path.join(HERE, "ab-report.md"), "w", encoding="utf-8").write("\n".join(out))
print("\n".join(out))
