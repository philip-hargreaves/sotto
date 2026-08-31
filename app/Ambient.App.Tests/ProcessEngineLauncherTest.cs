using Ambient.App.Core.Hosting;

namespace Ambient.App.Tests;

[Trait("Category", "Integration")]
public class ProcessEngineLauncherTest
{
    private static readonly TimeSpan ExitWait = TimeSpan.FromSeconds(5);

    private static ProcessEngineLauncher PingLauncher() =>
        new(Path.Combine(Environment.SystemDirectory, "ping.exe"), "-n 60 127.0.0.1");

    private static TaskCompletionSource WatchExit(IEngineProcess process)
    {
        var exited = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        process.Exited += () => exited.TrySetResult();
        return exited;
    }

    [Fact]
    public async Task LaunchedProcessRunsAndReportsItsDeath()
    {
        using var launcher = PingLauncher();
        using var process = launcher.Launch();
        var exited = WatchExit(process);
        Assert.False(process.HasExited);

        process.Kill();

        await exited.Task.WaitAsync(ExitWait);
        Assert.True(process.HasExited);
        Assert.Equal(1, process.ExitCode);
    }

    [Fact]
    public async Task DisposingTheLauncherKillsTheEngine()
    {
        var launcher = PingLauncher();
        using var process = launcher.Launch();
        var exited = WatchExit(process);

        launcher.Dispose();

        await exited.Task.WaitAsync(ExitWait);
        Assert.True(process.HasExited);
    }

    [Fact]
    public async Task ExtraArgumentsJoinTheCommandLine()
    {
        // Split across base and extra; a mangled join exits at once
        using var launcher = new ProcessEngineLauncher(
            Path.Combine(Environment.SystemDirectory, "ping.exe"), "-n",
            extraArguments: () => "60 127.0.0.1");
        using var process = launcher.Launch();
        var exited = WatchExit(process);

        await Task.Delay(500);
        Assert.False(process.HasExited);
        process.Kill();
        await exited.Task.WaitAsync(ExitWait);
    }

    [Fact]
    public void MissingExecutableThrows()
    {
        using var launcher = new ProcessEngineLauncher(@"C:\does\not\exist\engine.exe");

        Assert.ThrowsAny<Exception>(launcher.Launch);
    }
}
