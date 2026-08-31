using System.Diagnostics;
using Sotto.App.Core.Hosting;

namespace Sotto.App.Tests;

/// <summary>
/// The assembled supervision loop on real processes: a stand-in engine is
/// killed externally, exactly like Task Manager, and the recovery observed.
/// </summary>
[Trait("Category", "Integration")]
public class SupervisionIntegrationTest
{
    private static readonly TimeSpan Wait = TimeSpan.FromSeconds(10);

    private sealed class RecordingLauncher(IEngineLauncher inner) : IEngineLauncher
    {
        public List<IEngineProcess> Launched { get; } = [];

        public IEngineProcess Launch()
        {
            var process = inner.Launch();
            Launched.Add(process);
            return process;
        }
    }

    private sealed class FakeSession : ISessionState
    {
        public bool ConsultationActive { get; set; }
    }

    private sealed class Rig : IDisposable
    {
        private readonly ProcessEngineLauncher _inner = new(
            Path.Combine(Environment.SystemDirectory, "ping.exe"), "-n 60 127.0.0.1");

        private readonly string _directory =
            Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());

        public RecordingLauncher Launcher { get; }

        public FakeSession Session { get; } = new();

        public string CrashPath { get; }

        public EngineSupervisor Host { get; }

        public Rig()
        {
            CrashPath = Path.Combine(_directory, "crashes.jsonl");
            Launcher = new RecordingLauncher(_inner);
            Host = new EngineSupervisor(
                Launcher, Session, TimeProvider.System, new FileCrashLog(CrashPath));
        }

        public int Pid(int launchIndex) => Launcher.Launched[launchIndex].Id;

        public void Dispose()
        {
            Host.Dispose();
            _inner.Dispose();
            if (Directory.Exists(_directory))
            {
                Directory.Delete(_directory, recursive: true);
            }
        }
    }

    private static Task WhenStatusAsync(IEngineHost host, EngineStatus wanted)
    {
        var tcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        host.StatusChanged += OnChanged;
        return tcs.Task;

        void OnChanged(EngineStatus status)
        {
            if (status == wanted)
            {
                host.StatusChanged -= OnChanged;
                tcs.TrySetResult();
            }
        }
    }

    private static async Task WaitUntilGoneAsync(int pid)
    {
        for (var i = 0; i < 100; i++)
        {
            try
            {
                using var process = Process.GetProcessById(pid);
            }
            catch (ArgumentException)
            {
                return;
            }

            await Task.Delay(50);
        }

        Assert.Fail($"process {pid} is still running");
    }

    [Fact]
    public async Task KillingTheEngineTriggersARealRestart()
    {
        using var rig = new Rig();
        rig.Host.Start();
        var firstPid = rig.Pid(0);

        var restarted = WhenStatusAsync(rig.Host, EngineStatus.Running);
        Process.GetProcessById(firstPid).Kill();
        await restarted.WaitAsync(Wait);

        Assert.Equal(2, rig.Launcher.Launched.Count);
        var secondPid = rig.Pid(1);
        Assert.NotEqual(firstPid, secondPid);
        using (var replacement = Process.GetProcessById(secondPid))
        {
            Assert.False(replacement.HasExited);
        }

        Assert.Single(File.ReadAllLines(rig.CrashPath));

        rig.Host.Shutdown();
        await WaitUntilGoneAsync(secondPid);
    }

    [Fact]
    public async Task MidConsultationKillRestartsForResume()
    {
        using var rig = new Rig();
        rig.Host.Start();
        rig.Session.ConsultationActive = true;
        var firstPid = rig.Pid(0);

        var restarted = WhenStatusAsync(rig.Host, EngineStatus.Running);
        Process.GetProcessById(firstPid).Kill();
        await restarted.WaitAsync(Wait);

        // The stored audio makes a restart recoverable, so the death is
        // never surfaced as a fault mid-consultation
        Assert.Null(rig.Host.Fault);
        Assert.Equal(2, rig.Launcher.Launched.Count);
        Assert.NotEqual(firstPid, rig.Pid(1));
        Assert.Single(File.ReadAllLines(rig.CrashPath));

        rig.Host.Shutdown();
        await WaitUntilGoneAsync(rig.Pid(1));
    }

    [Fact]
    public async Task ShutdownLeavesNothingBehind()
    {
        using var rig = new Rig();
        rig.Host.Start();
        var pid = rig.Pid(0);

        rig.Host.Shutdown();

        await WaitUntilGoneAsync(pid);
        Assert.False(File.Exists(rig.CrashPath));
    }
}
