using Ambient.App.Core.Hosting;

namespace Ambient.App.Tests;

public sealed class LogRotationTest : IDisposable
{
    private readonly string _dir = Directory.CreateTempSubdirectory("ambient-logs").FullName;

    private string Log(string name) => Path.Combine(_dir, name);

    public void Dispose() => Directory.Delete(_dir, recursive: true);

    [Fact]
    public void EachRotationShiftsEveryRunDownOneSlot()
    {
        File.WriteAllText(Log("engine.log"), "run 1");
        LogRotation.Rotate(Log("engine.log"), keep: 3);
        File.WriteAllText(Log("engine.log"), "run 2");
        LogRotation.Rotate(Log("engine.log"), keep: 3);

        Assert.False(File.Exists(Log("engine.log")), "the current run starts fresh");
        Assert.Equal("run 2", File.ReadAllText(Log("engine-1.log")));
        Assert.Equal("run 1", File.ReadAllText(Log("engine-2.log")));
    }

    [Fact]
    public void TheOldestRunFallsOffAtTheKeepLimit()
    {
        for (var run = 1; run <= 4; run++)
        {
            File.WriteAllText(Log("engine.log"), $"run {run}");
            LogRotation.Rotate(Log("engine.log"), keep: 3);
        }

        Assert.Equal("run 4", File.ReadAllText(Log("engine-1.log")));
        Assert.Equal("run 3", File.ReadAllText(Log("engine-2.log")));
        Assert.False(File.Exists(Log("engine-3.log")), "keep bounds the set");
    }

    [Fact]
    public void AMissingLogRotatesToNothing()
    {
        LogRotation.Rotate(Log("engine.log"), keep: 3);

        Assert.Empty(Directory.GetFiles(_dir));
    }
}
