# Overnight 1x performance loop: drives the release engine over the pipe with
# replayed consults at real-time speed and records per-phase timings, live
# turn lag, engine memory and every engine death. Internal evidence tool.
import collections
import ctypes
import ctypes.wintypes
import json
import msvcrt
import os
import struct
import subprocess
import sys
import time
from datetime import datetime, timezone

ROOT = r"C:\dev\sotto"
ENGINE = os.path.join(ROOT, r"build\release\engine\sotto_engine.exe")
MODELS = os.path.join(ROOT, "models")
# Working data (tracks, store, logs, results) lives under build/, never in the repo
HERE = os.environ.get("PERF_DIR", os.path.join(ROOT, "build", "perf-loop"))
STORE = os.path.join(HERE, "store")
LOGS = os.path.join(HERE, "logs")
TAG = os.environ.get("PERF_TAG", "")  # experiments keep their own result files
RESULTS = os.path.join(HERE, f"runs{TAG}.jsonl")
EVENTS = os.path.join(HERE, f"events{TAG}.jsonl")
STOP_FILE = os.path.join(HERE, "STOP")

TRACKS = [
    ("c02m_elbow.wav", 120),
    ("c05m_elbow.wav", 300),
    ("c10m_weight.wav", 600),
    ("c15m_elbow_chest.wav", 902),
    ("c20m_weight_allergy.wav", 1200),
    ("cfull_elbow.wav", 542),  # the whole consult, for reading transcripts before/after
    ("cfull_day1_consultation01.wav", 458),
    ("cfull_day1_consultation09.wav", 537),
    ("cfull_day2_consultation07.wav", 458),
    ("cfull_day3_consultation03.wav", 697),
]
SAVE_DIR = os.path.join(HERE, "transcripts") if os.environ.get("PERF_SAVE") else None
MAX_HOURS = float(sys.argv[1]) if len(sys.argv) > 1 else 8.0
ONLY = sys.argv[2] if len(sys.argv) > 2 else None  # e.g. c02m to smoke-test
# Sweeps: replay faster than real time (bit-identical transcript per SpeedParityTest)
# over every wav in a directory, one engine for the lot
SPEED = float(os.environ.get("PERF_SPEED", "1.0"))
AUDIO_DIR = os.environ.get("PERF_AUDIO_DIR")
if AUDIO_DIR:
    import wave
    TRACKS = []
    for name in sorted(os.listdir(AUDIO_DIR)):
        if name.endswith(".wav") and not name.endswith(("_eq.wav", "_far.wav")):
            with wave.open(os.path.join(AUDIO_DIR, name)) as w:
                TRACKS.append((name, w.getnframes() / w.getframerate()))
NOTE_TIMEOUT = 600.0
STOP_TIMEOUT = 600.0


def now():
    return time.perf_counter()


def stamp():
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def log(line):
    print(f"{datetime.now().strftime('%H:%M:%S')} {line}", flush=True)


def event(kind, **fields):
    with open(EVENTS, "a", encoding="utf-8") as f:
        f.write(json.dumps({"t": stamp(), "kind": kind, **fields}) + "\n")


# --- Windows memory readings (no psutil dependency) --------------------------

class ProcessMemoryCounters(ctypes.Structure):
    _fields_ = [
        ("cb", ctypes.wintypes.DWORD),
        ("PageFaultCount", ctypes.wintypes.DWORD),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
        ("PrivateUsage", ctypes.c_size_t),
    ]


class MemoryStatusEx(ctypes.Structure):
    _fields_ = [
        ("dwLength", ctypes.wintypes.DWORD),
        ("dwMemoryLoad", ctypes.wintypes.DWORD),
        ("ullTotalPhys", ctypes.c_ulonglong),
        ("ullAvailPhys", ctypes.c_ulonglong),
        ("ullTotalPageFile", ctypes.c_ulonglong),
        ("ullAvailPageFile", ctypes.c_ulonglong),
        ("ullTotalVirtual", ctypes.c_ulonglong),
        ("ullAvailVirtual", ctypes.c_ulonglong),
        ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
    ]


def process_memory_mb(pid):
    handle = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)  # QUERY_LIMITED
    if not handle:
        return None
    try:
        counters = ProcessMemoryCounters()
        counters.cb = ctypes.sizeof(counters)
        if not ctypes.windll.psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
            return None
        return {"ws_mb": round(counters.WorkingSetSize / 2**20),
                "private_mb": round(counters.PrivateUsage / 2**20)}
    finally:
        ctypes.windll.kernel32.CloseHandle(handle)


def system_memory_mb():
    status = MemoryStatusEx()
    status.dwLength = ctypes.sizeof(status)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status))
    return {"avail_phys_mb": round(status.ullAvailPhys / 2**20),
            "avail_commit_mb": round(status.ullAvailPageFile / 2**20)}


def note_host_pids():
    out = subprocess.run(
        ["tasklist", "/FI", "IMAGENAME eq sotto_note_host.exe", "/FO", "CSV", "/NH"],
        capture_output=True, text=True).stdout
    pids = []
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0] == "sotto_note_host.exe":
            pids.append(int(parts[1]))
    return pids


# --- Engine process + pipe client -------------------------------------------

class EngineDied(Exception):
    pass


class Engine:
    # One synchronous pipe handle serialises reads and writes, so a blocking
    # read on another thread would stall every write. Single-threaded instead:
    # PeekNamedPipe says how much is waiting and only that much is read.
    def __init__(self, index):
        self.index = index
        self.pipe_name = f"LOCAL\\sotto-perf-{os.getpid()}-{index}"
        self.log_path = os.path.join(LOGS, f"engine-{index:03d}.log")
        self.log_offset = 0
        self.notifications = collections.deque()
        self.replies = {}
        self.buf = b""
        self.dead = False
        self.next_id = 0
        self.launched = now()
        self.stderr = open(self.log_path, "wb")
        self.proc = subprocess.Popen(
            [ENGINE, self.pipe_name, STORE, MODELS], stderr=self.stderr, stdout=subprocess.DEVNULL)
        pipe = "\\\\.\\pipe\\" + self.pipe_name
        for _ in range(600):
            if self.proc.poll() is not None:
                raise EngineDied(f"exited {self.proc.returncode} before the pipe appeared")
            try:
                self.f = open(pipe, "r+b", buffering=0)
                break
            except OSError:
                time.sleep(0.05)
        else:
            raise EngineDied("pipe never appeared")
        self.handle = msvcrt.get_osfhandle(self.f.fileno())
        self.pipe_up = now()

    def _available(self):
        avail = ctypes.wintypes.DWORD(0)
        ok = ctypes.windll.kernel32.PeekNamedPipe(
            ctypes.c_void_p(self.handle), None, 0, None, ctypes.byref(avail), None)
        if not ok:
            self.dead = True
            raise EngineDied(f"pipe broke (error {ctypes.GetLastError()})")
        return avail.value

    def _pump(self):
        # Read what is waiting, parse whole frames; returns True if anything arrived
        avail = self._available()
        if avail == 0:
            if self.proc.poll() is not None:
                self.dead = True
                raise EngineDied(f"engine exited {self.proc.returncode}")
            return False
        chunk = self.f.read(min(avail, 1 << 20))
        if not chunk:
            self.dead = True
            raise EngineDied("pipe closed")
        self.buf += chunk
        t = now()
        while len(self.buf) >= 4:
            (length,) = struct.unpack("<I", self.buf[:4])
            if len(self.buf) < 4 + length:
                break
            msg = json.loads(self.buf[4:4 + length])
            self.buf = self.buf[4 + length:]
            if msg.get("id") is not None and "method" not in msg:
                self.replies[msg["id"]] = msg
            else:
                self.notifications.append((t, msg))
        return True

    def next_notification(self, timeout):
        deadline = now() + timeout
        while True:
            if self.notifications:
                return self.notifications.popleft()
            if not self._pump():
                if now() >= deadline:
                    return None
                time.sleep(0.005)

    def request(self, method, params=None, timeout=15.0):
        if self.dead:
            raise EngineDied("connection lost")
        self.next_id += 1
        rid = self.next_id
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        payload = json.dumps(msg).encode()
        t0 = now()
        try:
            self.f.write(struct.pack("<I", len(payload)) + payload)
        except OSError as e:
            self.dead = True
            raise EngineDied(f"write failed: {e}")
        deadline = t0 + timeout
        while rid not in self.replies:
            if not self._pump():
                if now() >= deadline:
                    raise TimeoutError(f"{method} did not answer in {timeout} s")
                time.sleep(0.005)
        reply = self.replies.pop(rid)
        if "error" in reply:
            raise RuntimeError(f"{method}: {reply['error']}")
        return reply.get("result"), now() - t0

    def exit_code(self):
        return self.proc.poll()

    def new_log_lines(self):
        with open(self.log_path, "rb") as f:
            f.seek(self.log_offset)
            data = f.read()
            self.log_offset = f.tell()
        return data.decode("utf-8", "replace").splitlines()

    def close(self):
        try:
            self.f.close()
        except Exception:
            pass
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.stderr.close()


# --- One replayed consultation -----------------------------------------------

def percentile(values, p):
    if not values:
        return None
    ordered = sorted(values)
    k = (len(ordered) - 1) * p
    lo, hi = int(k), min(int(k) + 1, len(ordered) - 1)
    return round(ordered[lo] + (ordered[hi] - ordered[lo]) * (k - lo), 2)


def run_session(engine, track, duration, cycle, run_index, tags=None, on_stop=None):
    path = os.path.join(AUDIO_DIR or os.path.join(HERE, "audio"), track)
    rec = {
        "t": stamp(), "cycle": cycle, "run": run_index, "engine": engine.index,
        **(tags or {}),
        "engine_uptime_s": round(now() - engine.launched, 1),
        "track": track, "audio_s": duration, "outcome": "ok", "error": None,
        "mem_before": process_memory_mb(engine.proc.pid), "sys_before": system_memory_mb(),
    }
    turns = []  # (wall_offset, first_frame, frame_count, speaker, text)
    levels = 0
    interrupted = None
    note_first = note_ready = patient_first = patient_ready = None
    note_text = patient_text = ""
    note_failed = patient_failed = None

    def drain(until, stop_on=None):
        # Consume notifications until wall time `until` or a named method
        nonlocal levels, interrupted, note_first, note_ready, patient_first
        nonlocal patient_ready, note_text, patient_text, note_failed, patient_failed
        while True:
            remaining = until - now()
            if remaining <= 0:
                return None
            item = engine.next_notification(min(remaining, 1.0))
            if item is None:
                continue
            t, msg = item
            method = msg.get("method")
            params = msg.get("params", {}) or {}
            if method == "audio.level":
                levels += 1
            elif method == "transcript.turn":
                turns.append((t - t_start, params.get("firstFrame", 0),
                              params.get("frameCount", 0), params.get("speaker", ""),
                              len(params.get("text", ""))))
            elif method == "session/interrupted":
                interrupted = params
            elif method == "note/partial":
                note_first = note_first or t
                note_text = params.get("text", note_text)
            elif method == "note/ready":
                note_first = note_first or t
                note_ready = t
                note_text = params.get("text", note_text)
            elif method == "note/failed":
                note_failed = params.get("detail", "?")
                note_ready = t
            elif method == "patient/partial":
                patient_first = patient_first or t
                patient_text = params.get("text", patient_text)
            elif method == "patient/ready":
                patient_first = patient_first or t
                patient_ready = t
                patient_text = params.get("text", patient_text)
            elif method == "patient/failed":
                patient_failed = params.get("detail", "?")
                patient_ready = t
            if stop_on and method in stop_on:
                return method

    try:
        t_start = now()
        result, rtt = engine.request(
            "session/start", {"replay": {"path": path, "speed": SPEED, "monitor": False}}, 30)
        session_id = result.get("sessionId")
        rec["session_id"] = session_id
        rec["start_rtt_s"] = round(rtt, 3)
        rec["replay_speed"] = SPEED
        # Real time: the source completes on its own at the end of the file
        drain(t_start + duration / SPEED + 1.5)
        rec["mem_live_end"] = process_memory_mb(engine.proc.pid)
        if interrupted:
            rec["outcome"] = "interrupted"
            rec["error"] = json.dumps(interrupted)
        # Stop blocks through finalise: its round trip is the finalise time
        t_stop = now()
        stop_probe = on_stop() if on_stop else None  # e.g. a clock sampler
        _, stop_rtt = engine.request("session/stop", None, STOP_TIMEOUT)
        if stop_probe:
            rec["stop_probe"] = stop_probe()
        rec["stop_at_s"] = round(t_stop - t_start, 1)
        rec["finalise_s"] = round(stop_rtt, 2)
        # Note lane
        reached = drain(t_stop + NOTE_TIMEOUT, stop_on={"note/ready", "note/failed"})
        if reached is None:
            rec["outcome"] = "note_timeout"
        else:
            reached = drain(t_stop + NOTE_TIMEOUT, stop_on={"patient/ready", "patient/failed"})
            if reached is None:
                rec["outcome"] = "patient_timeout"
        t_end = now()
        rec["note_first_token_s"] = round(note_first - t_stop - stop_rtt, 2) if note_first else None
        rec["note_done_s"] = round(note_ready - t_stop - stop_rtt, 2) if note_ready else None
        rec["patient_first_token_s"] = (
            round(patient_first - note_ready, 2) if patient_first and note_ready else None)
        rec["patient_done_s"] = round(patient_ready - note_ready, 2) if patient_ready and note_ready else None
        rec["stop_to_all_done_s"] = round(t_end - t_stop, 2)
        rec["note_chars"] = len(note_text)
        rec["patient_chars"] = len(patient_text)
        rec["note_failed"] = note_failed
        rec["patient_failed"] = patient_failed
        if note_failed or patient_failed:
            rec["outcome"] = "note_failed" if note_failed else "patient_failed"
        # Engine-side numbers for this session (Take() resets them)
        try:
            metrics, _ = engine.request("engine/metrics", None, 15)
            rec["metrics"] = {k: metrics.get(k) for k in (
                "stageSeconds", "loadSeconds", "asrRealtimeFactor", "audioSeconds",
                "lostFrames", "diarTicks", "turns", "clusters", "replaySpeed")}
        except Exception as e:
            rec["metrics_error"] = str(e)
        rec["mem_after"] = process_memory_mb(engine.proc.pid)
        rec["sys_after"] = system_memory_mb()
        rec["note_hosts"] = len(note_host_pids())
        # For reading before/after: the sealed transcript, note and sheet
        if SAVE_DIR:
            try:
                os.makedirs(SAVE_DIR, exist_ok=True)
                sealed, _ = engine.request("session/transcript", {"id": session_id}, 30)
                stored_note, _ = engine.request("session/note", {"id": session_id}, 30)
                stored_patient, _ = engine.request("session/patient", {"id": session_id}, 30)
                base = os.path.join(SAVE_DIR, f"{TAG.lstrip('-') or 'run'}-{track.rsplit('.', 1)[0]}")
                with open(base + ".json", "w", encoding="utf-8") as f:
                    json.dump({"turns": sealed.get("turns", []), "note": stored_note.get("text"),
                               "patient": stored_patient.get("text")}, f, indent=1)
                with open(base + ".txt", "w", encoding="utf-8") as f:
                    for t in sealed.get("turns", []):
                        f.write(f"[{t['firstFrame'] / 16000:7.1f}s] {t['speaker'] or '?':8s} {t['text']}\n")
                    f.write("\n=== NOTE ===\n" + (stored_note.get("text") or "") + "\n")
                    f.write("\n=== PATIENT ===\n" + (stored_patient.get("text") or "") + "\n")
                rec["saved"] = base
            except Exception as e:
                rec["save_error"] = str(e)
        # Keep the store small; delete exercises the path too
        try:
            engine.request("session/delete", {"id": session_id}, 30)
        except Exception as e:
            rec["delete_error"] = str(e)
    except EngineDied as e:
        rec["outcome"] = "engine_died"
        rec["error"] = str(e)
        rec["exit_code"] = engine.exit_code()
    except TimeoutError as e:
        rec["outcome"] = "rpc_timeout"
        rec["error"] = str(e)
    except Exception as e:
        rec["outcome"] = "error"
        rec["error"] = f"{type(e).__name__}: {e}"

    # Live lag: how far behind real time each turn arrived
    lags = [round(w - (ff + fc) / 16000.0, 2) for w, ff, fc, _, _ in turns]
    rec["turns_live"] = len(turns)
    rec["levels"] = levels
    rec["lag_median_s"] = percentile(lags, 0.5)
    rec["lag_p95_s"] = percentile(lags, 0.95)
    rec["lag_max_s"] = max(lags) if lags else None
    rec["speakers"] = sorted({s for _, _, _, s, _ in turns})
    # The engine's own finalise stage lines for this run
    stages = {}
    extra = []
    for line in engine.new_log_lines():
        if "finalise " in line and " at " in line:
            try:
                name = line.split("finalise ", 1)[1].rsplit(" at ", 1)[0]
                stages[name] = float(line.rsplit(" at ", 1)[1].rstrip(" s"))
            except ValueError:
                pass
        elif "sotto-engine:" in line and ("ready in" not in line):
            extra.append(line.split("sotto-engine: ", 1)[-1][:160])
    rec["finalise_stages"] = stages
    rec["engine_log"] = extra[-12:]
    with open(RESULTS, "a", encoding="utf-8") as f:
        f.write(json.dumps(rec) + "\n")
    log(f"  {track:24s} {rec['outcome']:12s} finalise {rec.get('finalise_s')} s  "
        f"note {rec.get('note_first_token_s')}/{rec.get('note_done_s')} s  "
        f"sheet {rec.get('patient_done_s')} s  lag med {rec['lag_median_s']} p95 {rec['lag_p95_s']}  "
        f"turns {len(turns)}  ws {rec.get('mem_after', {}) and rec['mem_after'].get('ws_mb')} MB")
    return rec["outcome"]


# --- Loop --------------------------------------------------------------------

def start_engine(index):
    for attempt in range(5):
        try:
            return _start_engine(index + attempt * 1000)
        except (EngineDied, TimeoutError, RuntimeError) as e:
            event("engine_start_failed", index=index, attempt=attempt, error=str(e))
            log(f"engine start failed ({e}); retrying")
            time.sleep(10)
    raise RuntimeError("engine would not start")


def _start_engine(index):
    t0 = now()
    engine = Engine(index)
    for _ in range(600):
        try:
            engine.request("engine/echo", {"payload": "up"}, 2)
            break
        except (TimeoutError, RuntimeError):
            time.sleep(0.1)
    echo_at = now() - t0
    # Readiness: the compile caches; loads still proceed in the background
    ready_at = None
    for _ in range(1200):
        result, _ = engine.request("engine/readiness", None, 10)
        if result.get("ready"):
            ready_at = now() - t0
            break
        time.sleep(0.5)
    event("engine_started", index=index, pid=engine.proc.pid, echo_s=round(echo_at, 2),
          ready_s=round(ready_at, 2) if ready_at else None, mem=process_memory_mb(engine.proc.pid))
    log(f"engine {index} pid {engine.proc.pid}: echo {echo_at:.2f} s, ready {ready_at and round(ready_at, 1)} s")
    return engine


def main():
    os.makedirs(STORE, exist_ok=True)
    os.makedirs(LOGS, exist_ok=True)
    tracks = [t for t in TRACKS if ONLY is None or t[0].startswith(ONLY)]
    deadline = now() + MAX_HOURS * 3600
    event("loop_started", max_hours=MAX_HOURS, tracks=[t[0] for t in tracks])
    engine_index = 0
    engine = None
    cycle = 0
    run_index = 0
    crashes = 0
    try:
        max_cycles = int(os.environ.get("PERF_CYCLES", "0"))  # 0: until the deadline
        while now() < deadline and not os.path.exists(STOP_FILE) \
                and (max_cycles == 0 or cycle < max_cycles):
            cycle += 1
            # A fresh engine per cycle: cold start is measured every cycle and a
            # leaking engine cannot poison the whole night
            if engine is not None:
                engine.close()
            engine_index += 1
            engine = start_engine(engine_index)
            # Warm the note model the way the app does: it loads in the
            # background during the first capture, so the first run is cold
            log(f"cycle {cycle}")
            for track, duration in tracks:
                if now() >= deadline or os.path.exists(STOP_FILE):
                    break
                run_index += 1
                outcome = run_session(engine, track, duration, cycle, run_index)
                if outcome == "engine_died" or engine.exit_code() is not None:
                    crashes += 1
                    code = engine.exit_code()
                    event("engine_died", index=engine.index, exit_code=code,
                          uptime_s=round(now() - engine.launched, 1), run=run_index, track=track,
                          log_tail=engine.new_log_lines()[-15:])
                    log(f"ENGINE DIED exit {code} (crash #{crashes}); relaunching")
                    engine.close()
                    engine_index += 1
                    engine = start_engine(engine_index)
                time.sleep(3)
    finally:
        if engine is not None:
            engine.close()
        event("loop_finished", cycles=cycle, runs=run_index, crashes=crashes)
        log(f"done: {cycle} cycles, {run_index} runs, {crashes} engine deaths")


if __name__ == "__main__":
    main()
