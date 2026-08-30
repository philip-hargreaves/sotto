# perf-loop: 1x performance harness

Internal evidence tool, not shipped. Drives the release engine over its pipe with replayed
consults at real-time speed and records per-phase timings, live turn lag, engine memory and
every engine death. Findings and data from the first campaign (2026-08-29) live in the
research repo: `docs/production/latency-probe-2026-08-29.md`.

All working data goes to `build/perf-loop/` (override with `PERF_DIR`); nothing here writes
into the repo.

```
python tools/perf-loop/make_tracks.py            # 2/5/10/15/20-minute tracks from demo/
python tools/perf-loop/perf_loop.py 9            # soak: cycles of all tracks for 9 h
python tools/perf-loop/perf_loop.py 0.01 c02m    # one 2-minute run
python tools/perf-loop/analyse.py                # -> build/perf-loop/report.md
python tools/perf-loop/ab_test.py                # EcoQoS A/B/C, fresh engine per run
python tools/perf-loop/ab_report.py              # -> build/perf-loop/ab-report.md
```

Transcript quality gate (method: research repo `docs/production/transcript-quality-gate.md`):

```
# before and after a change, all 57 consults at 16x, transcripts saved per consult
$env:PERF_TAG='-before'; $env:PERF_SAVE='1'; $env:PERF_SPEED='16'; $env:PERF_CYCLES='1'
$env:PERF_AUDIO_DIR='C:\dev\intelliscribe\bench\transcription\mixed'
python tools/perf-loop/perf_loop.py 3
python tools/perf-loop/score_gate.py before after   # WER, negation diffs, attribution
```

Rules that the numbers depend on: one model-loading job on the machine at a time; state the
power mode and whether the machine was attended (ADR-0028); create `build/perf-loop/STOP` to
end a loop between runs. `PERF_TAG=-name` keeps an experiment's results in their own files.

The pipe client is single-threaded on purpose: a synchronous named-pipe handle serialises
reads and writes, so it polls `PeekNamedPipe` and reads only what is waiting.
