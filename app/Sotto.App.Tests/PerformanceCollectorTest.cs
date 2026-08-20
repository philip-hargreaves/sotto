using System.Text.Json;
using Sotto.App.Core.Metrics;

namespace Sotto.App.Tests;

public class PerformanceCollectorTest : IDisposable
{
    private readonly string _path = Path.Combine(
        Path.GetTempPath(), Path.GetRandomFileName(), "metrics.jsonl");

    public void Dispose()
    {
        try
        {
            Directory.Delete(Path.GetDirectoryName(_path)!, recursive: true);
        }
        catch (IOException)
        {
        }

        GC.SuppressFinalize(this);
    }

    private PerformanceCollector NewCollector(FakeEngineClient engine, bool enabled = true) =>
        new(engine, () => enabled, () => null, _path);

    [Fact]
    public async Task AFinishedSessionAppendsOneLine()
    {
        var engine = new FakeEngineClient();
        var collector = NewCollector(engine);

        collector.SessionStarted("replay", 1.0, "Elbow swelling");
        collector.StopRequested();
        collector.NotePartial();
        collector.NotePartial();
        await collector.SessionFinishedAsync(null, 1290);

        var line = Assert.Single(File.ReadAllLines(_path));
        using var record = JsonDocument.Parse(line);
        var root = record.RootElement;
        Assert.Equal("replay", root.GetProperty("source").GetString());
        Assert.Equal(1.0, root.GetProperty("replaySpeed").GetDouble());
        Assert.Equal("Elbow swelling", root.GetProperty("track").GetString());
        Assert.Equal(33.4, root.GetProperty("engine").GetProperty("asrRealtimeFactor").GetDouble());
        Assert.Equal(1290, root.GetProperty("note").GetProperty("chars").GetInt32());
        Assert.True(root.GetProperty("note").GetProperty("firstPartialAfterStopSeconds")
            .GetDouble() >= 0);
        Assert.True(root.GetProperty("memory").GetProperty("availableAtStartMb").GetInt64() > 0);
    }

    [Fact]
    public async Task DisabledCollectionWritesNothing()
    {
        var collector = NewCollector(new FakeEngineClient(), enabled: false);

        collector.SessionStarted("mic", 0, null);
        collector.StopRequested();
        await collector.SessionFinishedAsync(null, 100);

        Assert.False(File.Exists(_path));
    }

    [Fact]
    public async Task AFailedNoteIsRecordedWithItsReason()
    {
        var engine = new FakeEngineClient();
        var collector = NewCollector(engine);

        collector.SessionStarted("mic", 0, null);
        collector.StopRequested();
        await collector.SessionFinishedAsync("the transcript is empty", 0);
        collector.SessionStarted("mic", 0, null);
        collector.StopRequested();
        await collector.SessionFinishedAsync(null, 42);

        var lines = File.ReadAllLines(_path);
        Assert.Equal(2, lines.Length);
        using var failed = JsonDocument.Parse(lines[0]);
        Assert.Equal("the transcript is empty",
            failed.RootElement.GetProperty("note").GetProperty("failed").GetString());
        using var fine = JsonDocument.Parse(lines[1]);
        Assert.False(fine.RootElement.GetProperty("note").TryGetProperty("failed", out _));
    }

    [Fact]
    public async Task AFinishWithoutAStopIsIgnored()
    {
        var collector = NewCollector(new FakeEngineClient());
        await collector.SessionFinishedAsync(null, 5);
        Assert.False(File.Exists(_path));
    }
}
