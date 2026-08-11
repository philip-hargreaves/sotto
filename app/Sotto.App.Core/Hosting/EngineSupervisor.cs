namespace Sotto.App.Core.Hosting;

/// <summary>
/// Runs the engine and applies RestartPolicy when it dies unexpectedly.
/// Status events are raised outside the lock, after the state has settled.
/// </summary>
public sealed class EngineSupervisor(
    IEngineLauncher launcher, ISessionState session, TimeProvider clock, ICrashLog crashLog)
    : IEngineHost, IDisposable
{
    private readonly object _gate = new();
    private readonly List<DateTimeOffset> _crashes = [];
    private IEngineProcess? _process;
    private DateTimeOffset _launchedAt;

    public event Action<EngineStatus>? StatusChanged;

    public EngineStatus Status { get; private set; } = EngineStatus.Stopped;

    public EngineFault? Fault { get; private set; }

    public void Start()
    {
        var changes = new List<EngineStatus>();
        lock (_gate)
        {
            if (_process is not null)
            {
                return;
            }

            Fault = null;
            _crashes.Clear();
            LaunchLocked(changes);
        }

        Raise(changes);
    }

    public void Shutdown()
    {
        IEngineProcess? process;
        var changes = new List<EngineStatus>();
        lock (_gate)
        {
            if (Status == EngineStatus.Stopped)
            {
                return;
            }

            process = _process;
            _process = null;
            Fault = null;
            SetStatusLocked(EngineStatus.Stopped, changes);
        }

        process?.Kill();
        process?.Dispose();
        Raise(changes);
    }

    public void Dispose() => Shutdown();

    private void LaunchLocked(List<EngineStatus> changes)
    {
        IEngineProcess process;
        try
        {
            process = launcher.Launch();
        }
        catch (Exception)
        {
            Fault = new EngineFault(EngineFaultKind.LaunchFailed);
            SetStatusLocked(EngineStatus.Faulted, changes);
            return;
        }

        _process = process;
        _launchedAt = clock.GetUtcNow();
        process.Exited += () => OnExited(process);
        SetStatusLocked(EngineStatus.Running, changes);

        // It may have died before the handler was attached
        if (process.HasExited)
        {
            HandleExitLocked(process, changes);
        }
    }

    private void OnExited(IEngineProcess process)
    {
        var changes = new List<EngineStatus>();
        lock (_gate)
        {
            HandleExitLocked(process, changes);
        }

        Raise(changes);
    }

    private void HandleExitLocked(IEngineProcess process, List<EngineStatus> changes)
    {
        // Stale event: a deliberate stop or an earlier path already handled it
        if (!ReferenceEquals(process, _process))
        {
            return;
        }

        _process = null;
        var exitCode = process.ExitCode;
        process.Dispose();

        var now = clock.GetUtcNow();
        _crashes.RemoveAll(crash => now - crash > RestartPolicy.StormWindow);
        _crashes.Add(now);

        var action = RestartPolicy.Decide(session.ConsultationActive, _crashes, now);
        crashLog.Record(new CrashReport(now, exitCode, now - _launchedAt, _crashes.Count, action));

        switch (action)
        {
            case RecoveryAction.Restart:
                SetStatusLocked(EngineStatus.Restarting, changes);
                LaunchLocked(changes);
                break;
            case RecoveryAction.Surface:
                Fault = new EngineFault(EngineFaultKind.SessionInterrupted, exitCode);
                SetStatusLocked(EngineStatus.Faulted, changes);
                break;
            default:
                Fault = new EngineFault(EngineFaultKind.CrashLoop, exitCode);
                SetStatusLocked(EngineStatus.Faulted, changes);
                break;
        }
    }

    private void SetStatusLocked(EngineStatus status, List<EngineStatus> changes)
    {
        Status = status;
        changes.Add(status);
    }

    private void Raise(List<EngineStatus> changes)
    {
        foreach (var status in changes)
        {
            StatusChanged?.Invoke(status);
        }
    }
}
