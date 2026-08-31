using System.Text.Json;
using Sotto.App.Core.ViewModels;

namespace Sotto.App.Tests;

/// <summary>The live numbers behind the status bar's model chips.</summary>
public class StatusBarMetricsTest
{
    private static JsonElement Params(object value) => JsonSerializer.SerializeToElement(value);

    private static (StatusBarViewModel Status, FakeEngineClient Engine) Create()
    {
        var engine = new FakeEngineClient(autoNotify: false);
        return (new StatusBarViewModel(engine, new InlineDispatcher()), engine);
    }

    [Fact]
    public void PartialsFromAnyLaneDriveTheMeter()
    {
        var (status, engine) = Create();
        Assert.False(status.TokensStreaming);

        engine.RaiseNotification("note/partial", Params(new { text = "The" }));
        Assert.True(status.TokensStreaming);

        engine.RaiseNotification("note/ready");
        Assert.False(status.TokensStreaming, "the stream ended; the value holds");

        engine.RaiseNotification("translate/partial", Params(new { text = "Twoja" }));
        Assert.True(status.TokensStreaming, "a translation meters the same way");
        engine.RaiseNotification("translate/failed");
        Assert.False(status.TokensStreaming);
    }

    [Fact]
    public void ResetClearsTheMeterForANewConsultation()
    {
        var (status, engine) = Create();
        engine.RaiseNotification("note/partial", Params(new { text = "The" }));
        engine.RaiseNotification("note/ready");

        status.ResetThroughput();

        Assert.False(status.TokensStreaming);
        Assert.Equal(0, status.TokensPerSecond);
    }

    [Fact]
    public async Task PollingReadsTheRealtimeFactor()
    {
        var (status, engine) = Create();
        Assert.Equal(0, status.RealtimeFactor);

        await status.PollMetricsOnceAsync();

        Assert.Equal(33.4, status.RealtimeFactor);  // FakeEngineClient's figure
        Assert.False(status.RealtimeLow, "not recording: no warning");
    }

    [Fact]
    public async Task ALowFactorWarnsOnlyWhileRecording()
    {
        var (status, engine) = Create();
        await status.PollMetricsOnceAsync();

        status.SetMicVisible(true);
        Assert.False(status.RealtimeLow, "33x is healthy");

        // The next poll finds transcription barely keeping up
        engine.MetricsRealtimeFactor = 1.4;
        await status.PollMetricsOnceAsync();
        Assert.True(status.RealtimeLow);

        status.SetMicVisible(false);
        Assert.False(status.RealtimeLow, "the warning is a recording-time signal");
    }
}
