using Sotto.App.Core.Hosting;

namespace Sotto.App.Tests;

public class EngineSupervisorTest
{
    private sealed class FakeProcess(bool stillborn = false) : IEngineProcess
    {
        public event Action? Exited;

        public int Id => 1234;

        public bool HasExited { get; private set; } = stillborn;

        public int ExitCode { get; private set; }

        public bool Killed { get; private set; }

        public void Kill()
        {
            Killed = true;
            Crash(1);
        }

        public void Crash(int exitCode)
        {
            HasExited = true;
            ExitCode = exitCode;
            Exited?.Invoke();
        }

        public void Dispose()
        {
        }
    }

    private sealed class FakeLauncher : IEngineLauncher
    {
        public List<FakeProcess> Launched { get; } = [];

        public bool FailNext { get; set; }

        public bool NextIsStillborn { get; set; }

        public IEngineProcess Launch()
        {
            if (FailNext)
            {
                FailNext = false;
                throw new InvalidOperationException("no engine");
            }

            var process = new FakeProcess(NextIsStillborn);
            NextIsStillborn = false;
            Launched.Add(process);
            return process;
        }
    }

    private sealed class FakeSession : ISessionState
    {
        public bool ConsultationActive { get; set; }
    }

    private sealed class TestClock : TimeProvider
    {
        public DateTimeOffset Now { get; set; } = DateTimeOffset.UnixEpoch;

        public override DateTimeOffset GetUtcNow() => Now;
    }

    private sealed class Harness
    {
        public FakeLauncher Launcher { get; } = new();

        public FakeSession Session { get; } = new();

        public TestClock Clock { get; } = new();

        public List<EngineStatus> Statuses { get; } = [];

        public EngineSupervisor Host { get; }

        public Harness()
        {
            Host = new EngineSupervisor(Launcher, Session, Clock);
            Host.StatusChanged += Statuses.Add;
        }

        public FakeProcess Current => Launcher.Launched[^1];
    }

    [Fact]
    public void StartLaunchesTheEngine()
    {
        var h = new Harness();

        h.Host.Start();

        Assert.Equal(EngineStatus.Running, h.Host.Status);
        Assert.Single(h.Launcher.Launched);
        Assert.Equal(new[] { EngineStatus.Running }, h.Statuses);
    }

    [Fact]
    public void IdleCrashRestartsSilently()
    {
        var h = new Harness();
        h.Host.Start();

        h.Current.Crash(-1);

        Assert.Equal(EngineStatus.Running, h.Host.Status);
        Assert.Null(h.Host.Fault);
        Assert.Equal(2, h.Launcher.Launched.Count);
        Assert.Equal(
            new[] { EngineStatus.Running, EngineStatus.Restarting, EngineStatus.Running },
            h.Statuses);
    }

    [Fact]
    public void MidConsultationCrashIsSurfacedNotRestarted()
    {
        var h = new Harness();
        h.Host.Start();
        h.Session.ConsultationActive = true;

        h.Current.Crash(5);

        Assert.Equal(EngineStatus.Faulted, h.Host.Status);
        Assert.Equal(new EngineFault(EngineFaultKind.SessionInterrupted, 5), h.Host.Fault);
        Assert.Single(h.Launcher.Launched);
    }

    [Fact]
    public void CrashStormGivesUp()
    {
        var h = new Harness();
        h.Host.Start();

        for (var i = 0; i < RestartPolicy.StormLimit; i++)
        {
            h.Current.Crash(-1);
        }

        Assert.Equal(EngineStatus.Faulted, h.Host.Status);
        Assert.Equal(EngineFaultKind.CrashLoop, h.Host.Fault!.Kind);
        Assert.Equal(RestartPolicy.StormLimit, h.Launcher.Launched.Count);
    }

    [Fact]
    public void SpacedCrashesNeverTripTheStorm()
    {
        var h = new Harness();
        h.Host.Start();

        for (var i = 0; i < RestartPolicy.StormLimit + 3; i++)
        {
            h.Current.Crash(-1);
            h.Clock.Now += RestartPolicy.StormWindow + TimeSpan.FromSeconds(1);
        }

        Assert.Equal(EngineStatus.Running, h.Host.Status);
        Assert.Null(h.Host.Fault);
    }

    [Fact]
    public void ShutdownIsDeliberateNotACrash()
    {
        var h = new Harness();
        h.Host.Start();

        h.Host.Shutdown();

        Assert.Equal(EngineStatus.Stopped, h.Host.Status);
        Assert.True(h.Current.Killed);
        Assert.Single(h.Launcher.Launched);
        Assert.Null(h.Host.Fault);
    }

    [Fact]
    public void StartAfterAFaultRecovers()
    {
        var h = new Harness();
        h.Host.Start();
        for (var i = 0; i < RestartPolicy.StormLimit; i++)
        {
            h.Current.Crash(-1);
        }

        h.Host.Start();

        Assert.Equal(EngineStatus.Running, h.Host.Status);
        Assert.Null(h.Host.Fault);
        h.Current.Crash(-1);
        Assert.Equal(EngineStatus.Running, h.Host.Status);
    }

    [Fact]
    public void LaunchFailureIsAFault()
    {
        var h = new Harness();
        h.Launcher.FailNext = true;

        h.Host.Start();

        Assert.Equal(EngineStatus.Faulted, h.Host.Status);
        Assert.Equal(new EngineFault(EngineFaultKind.LaunchFailed), h.Host.Fault);
    }

    [Fact]
    public void DeathBeforeTheHandlerAttachesIsStillHandled()
    {
        var h = new Harness();
        h.Launcher.NextIsStillborn = true;

        h.Host.Start();

        Assert.Equal(EngineStatus.Running, h.Host.Status);
        Assert.Equal(2, h.Launcher.Launched.Count);
    }
}
