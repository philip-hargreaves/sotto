# Interleaved A/B/C test of Windows power throttling (EcoQoS) on the engine and
# note host. Fresh engine per run; the throttling state is applied to both
# processes and read back; CPU clocks are sampled during every finalise.
import ctypes, ctypes.wintypes, json, os, re, statistics, sys, threading, time
os.environ.setdefault("PERF_TAG", "-ab")
sys.argv = sys.argv[:1]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import perf_loop as pl

REPS = int(os.environ.get("AB_REPS", "3"))
TRACKS = [("c02m_elbow.wav", 120), ("c05m_elbow.wav", 300), ("c10m_weight.wav", 600)]
CONDITIONS = ["default", "eco_on", "eco_off"]

PROCESS_SET_INFORMATION = 0x0200
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
ProcessPowerThrottling = 4
EXECUTION_SPEED = 0x1
k32 = ctypes.windll.kernel32


class PowerThrottlingState(ctypes.Structure):
    _fields_ = [("Version", ctypes.wintypes.ULONG), ("ControlMask", ctypes.wintypes.ULONG),
                ("StateMask", ctypes.wintypes.ULONG)]


def set_throttling(pid, condition):
    if condition == "default":
        return "untouched"
    h = k32.OpenProcess(PROCESS_SET_INFORMATION, False, pid)
    if not h:
        return f"open failed {k32.GetLastError()}"
    state = PowerThrottlingState(1, EXECUTION_SPEED, EXECUTION_SPEED if condition == "eco_on" else 0)
    ok = k32.SetProcessInformation(h, ProcessPowerThrottling, ctypes.byref(state), ctypes.sizeof(state))
    err = k32.GetLastError()
    k32.CloseHandle(h)
    return "set" if ok else f"set failed {err}"


def read_throttling(pid):
    h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return None
    state = PowerThrottlingState(1, 0, 0)  # Version must be set for the read
    ok = k32.GetProcessInformation(h, ProcessPowerThrottling, ctypes.byref(state), ctypes.sizeof(state))
    k32.CloseHandle(h)
    if not ok:
        return None
    return {"control": state.ControlMask, "state": state.StateMask}


class ProcessorPowerInformation(ctypes.Structure):
    _fields_ = [("Number", ctypes.wintypes.ULONG), ("MaxMhz", ctypes.wintypes.ULONG),
                ("CurrentMhz", ctypes.wintypes.ULONG), ("MhzLimit", ctypes.wintypes.ULONG),
                ("MaxIdleState", ctypes.wintypes.ULONG), ("CurrentIdleState", ctypes.wintypes.ULONG)]


def cpu_mhz():
    n = os.cpu_count()
    buf = (ProcessorPowerInformation * n)()
    status = ctypes.windll.powrprof.CallNtPowerInformation(11, None, 0, ctypes.byref(buf), ctypes.sizeof(buf))
    if status != 0:
        return None
    return [c.CurrentMhz for c in buf], buf[0].MaxMhz


class ClockSampler:
    # CallNtPowerInformation's CurrentMhz is static on modern Windows, so the
    # PDH "% Processor Performance" counter (clock relative to nominal) is
    # sampled once a second via typeperf until stopped; no pipe access
    COUNTER = "\\Processor Information(_Total)\\% Processor Performance"

    def __call__(self):
        import subprocess
        self.proc = subprocess.Popen(
            ["typeperf", self.COUNTER, "-si", "1"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True)
        return self.finish

    def finish(self):
        self.proc.terminate()
        out, _ = self.proc.communicate(timeout=5)
        values = []
        for line in out.splitlines():
            parts = line.strip().strip('"').split('","')
            if len(parts) == 2:
                try:
                    values.append(float(parts[1]))
                except ValueError:
                    pass
        if not values:
            return None
        return {"samples": len(values), "perf_pct_mean": round(statistics.mean(values)),
                "perf_pct_min": round(min(values)), "perf_pct_max": round(max(values))}


def run_one(index, condition, track, duration, rep):
    engine = pl.Engine(index)
    engine.request("engine/echo", {"payload": "up"}, 10)
    engine_set = set_throttling(engine.proc.pid, condition)
    host = {"pid": None, "set": None, "found_after_s": None}
    t_run = time.time()
    last_check = [0.0]
    orig = engine.next_notification

    def watch(timeout):
        if host["pid"] is None and time.time() - last_check[0] > 1.0:
            last_check[0] = time.time()
            for pid in pl.note_host_pids():
                host["pid"] = pid
                host["set"] = set_throttling(pid, condition)
                host["found_after_s"] = round(time.time() - t_run, 1)
        return orig(timeout)

    engine.next_notification = watch
    sampler = ClockSampler()
    # A clock sample of the live phase, for contrast with the finalise sample
    live_clock = cpu_mhz()
    tags = {"condition": condition, "rep": rep, "engine_throttle_set": engine_set,
            "engine_throttle_read": read_throttling(engine.proc.pid),
            "live_mhz_mean_at_start": round(statistics.mean(live_clock[0])) if live_clock else None}
    outcome = pl.run_session(engine, track, duration, rep, index, tags=tags, on_stop=sampler)
    # Post-hoc facts for the record: host throttle read-back and the 9B load time
    post = {"host": host, "host_throttle_read": read_throttling(host["pid"]) if host["pid"] else None}
    log = open(engine.log_path, encoding="utf-8", errors="replace").read()
    m = re.search(r"note on GPU.*loaded in ([\d.]+) s", log)
    post["note_load_s"] = float(m.group(1)) if m else None
    with open(os.path.join(pl.HERE, "ab-post.jsonl"), "a", encoding="utf-8") as f:
        f.write(json.dumps({"run": index, "condition": condition, "track": track, **post}) + "\n")
    engine.close()
    pl.log(f"    [{condition}] engine={engine_set} host={host['set']}@{host['found_after_s']}s "
           f"load={post['note_load_s']}s")
    time.sleep(3)
    return outcome


def main():
    os.makedirs(pl.STORE, exist_ok=True)
    os.makedirs(pl.LOGS, exist_ok=True)
    index = 2000
    pl.event("ab_started", reps=REPS, tracks=[t[0] for t in TRACKS], conditions=CONDITIONS)
    for rep in range(REPS):
        for ti, (track, duration) in enumerate(TRACKS):
            # Rotate the condition order per (rep, track) so drift cannot align with a condition
            order = CONDITIONS[(rep + ti) % 3:] + CONDITIONS[:(rep + ti) % 3]
            for condition in order:
                if os.path.exists(pl.STOP_FILE):
                    pl.log("STOP file seen")
                    return
                index += 1
                pl.log(f"rep {rep + 1} {track} {condition}")
                run_one(index, condition, track, duration, rep + 1)
    pl.event("ab_finished")
    pl.log("A/B/C done")


if __name__ == "__main__":
    main()
