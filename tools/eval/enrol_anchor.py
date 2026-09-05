# Enrol a voiceprint through the real engine from a wav standing in for the microphone,
# and stage the resulting anchor for a sweep. Validates the enrolment path end to end and
# gives the 57-consultation gate an enrolled print to compare with the accrued one.
#   PERF_ENGINE=<engine exe> python tools/eval/enrol_anchor.py <doctor wav> <out anchor.bin> [seconds]
import os
import shutil
import subprocess
import sys
import time

wav, out = sys.argv[1], sys.argv[2]
seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 50.0

# The perf loop reads its own arguments at import
sys.argv = [sys.argv[0]]
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "perf-loop"))
import perf_loop  # noqa: E402

# The engine takes the replay wav as a fourth positional argument; the perf loop's
# launcher passes three, so the wav rides in through Popen
real_popen = subprocess.Popen


def popen_with_wav(args, **kwargs):
    if args and args[0] == perf_loop.ENGINE:
        args = [*args, wav]
    return real_popen(args, **kwargs)


subprocess.Popen = popen_with_wav
engine = perf_loop.Engine(9000)
for _ in range(600):  # up, without waiting on the note model's compile cache
    try:
        engine.request("engine/echo", {"payload": "up"}, 2)
        break
    except (TimeoutError, RuntimeError):
        time.sleep(0.1)
try:
    engine.request("anchor/clear")
    before, _ = engine.request("anchor/status")
    print("before:", before)
    engine.request("anchor/enrol", {"seconds": seconds})
    outcome = None
    t0 = time.time()
    progress = 0
    while outcome is None and time.time() - t0 < seconds + 120:
        engine._pump()
        while engine.notifications:
            _, msg = engine.notifications.popleft()
            method, params = msg.get("method"), msg.get("params", {})
            if method == "anchor/progress":
                progress += 1
            elif method == "anchor/enrolled":
                outcome = params
        time.sleep(0.02)
    print(f"progress notifications: {progress}")
    print("outcome:", outcome)
    after, _ = engine.request("anchor/status")
    print("after:", after)
    if not outcome or not outcome.get("ok"):
        sys.exit(1)
finally:
    engine.close()

shutil.copyfile(os.path.join(perf_loop.STORE, "anchor.bin"), out)
print("staged", out)
