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
    public async Task TheEngineMeasuredRateBeatsTheArrivalCount()
    {
        var (status, engine) = Create();
        await Task.Delay(50);

        // The engine meters at the source, before its 12 Hz throttle: the
        // shell shows that figure, not how often notifications arrived
        engine.RaiseNotification("note/partial",
            Params(new { text = "The", tokensPerSecond = 15.3 }));
        Assert.Equal(15.3, status.TokensPerSecond);
        Assert.Contains("15.3 tok/s", status.NoteChip);

        // The ready event carries the whole generation's average, which
        // holds and is labelled as what it is
        engine.RaiseNotification("note/ready",
            Params(new { text = "The note.", tokensPerSecond = 14.2 }));
        Assert.Equal(14.2, status.TokensPerSecond);
        Assert.False(status.TokensStreaming);
        Assert.Contains("Averaged 14.2 tok/s", status.NoteChip);
    }

    [Fact]
    public void WithoutTheEngineFigureTheArrivalMeterFallsBack()
    {
        var (status, engine) = Create();

        engine.RaiseNotification("note/partial", Params(new { text = "The" }));
        Assert.True(status.TokensStreaming);
        engine.RaiseNotification("note/failed");
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

        await status.PollMetricsOnceAsync();

        Assert.Equal(33.4, status.RealtimeFactor);  // FakeEngineClient's figure
        Assert.False(status.RealtimeLow, "not recording: no warning");
    }

    [Fact]
    public void FriendlyNamesDropPrecisionAndKeepSize()
    {
        Assert.Equal("Whisper Turbo", StatusBarViewModel.FriendlyModelName("whisper-turbo-int8"));
        Assert.Equal("Qwen3.5 9B", StatusBarViewModel.FriendlyModelName("qwen3.5-9b-int4"));
        Assert.Equal("Whisper Large V3",
            StatusBarViewModel.FriendlyModelName("whisper-large-v3-int8"));
    }

    [Fact]
    public async Task ChipsNameTheModelsAndCarryTheirLiveFigures()
    {
        var (status, engine) = Create();
        await Task.Delay(50);  // the connect-time model fetch

        Assert.Equal("Whisper Turbo · GPU", status.AsrChip);
        Assert.Equal("Qwen3.5 9B · GPU", status.NoteChip);

        // Recording: the realtime factor joins Whisper's chip
        status.SetMicVisible(true);
        await status.PollMetricsOnceAsync();
        Assert.Equal("Whisper Turbo · GPU · 33× RT", status.AsrChip);

        // A slow factor keeps a decimal
        engine.MetricsRealtimeFactor = 1.4;
        await status.PollMetricsOnceAsync();
        Assert.Equal("Whisper Turbo · GPU · 1.4× RT", status.AsrChip);

        // Stopped but still decoding the tail (the NPU's longest stage):
        // the figure stays until the transcript seals
        status.SetMicVisible(false);
        status.SetDecodeActive(true);
        Assert.Equal("Whisper Turbo · GPU · 1.4× RT", status.AsrChip);
        Assert.True(status.RealtimeLow);

        // Sealed: the session's average holds, labelled as what it is
        status.SetDecodeActive(false);
        Assert.Equal("Whisper Turbo · GPU · Averaged 1.4× RT", status.AsrChip);
        Assert.False(status.RealtimeLow);

        status.ResetThroughput();  // the next consultation starts clean
        Assert.Equal("Whisper Turbo · GPU", status.AsrChip);
    }

    [Fact]
    public async Task TheNoteChipMetersTheStream()
    {
        var (status, engine) = Create();
        await Task.Delay(50);
        Assert.DoesNotContain("tok/s", status.NoteChip);

        engine.RaiseNotification("note/partial", Params(new { text = "The" }));
        engine.RaiseNotification("note/partial", Params(new { text = "The patient" }));
        engine.RaiseNotification("note/ready");

        // Frozen value survives the stream's end; a new consultation clears it
        var frozen = status.NoteChip;
        status.ResetThroughput();
        Assert.Equal("Qwen3.5 9B · GPU", status.NoteChip);
        Assert.NotNull(frozen);
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

    [Fact]
    public void ChipDotsFollowTheWorkNotTheSession()
    {
        var (status, engine) = Create();
        Assert.False(status.AsrActive);
        Assert.False(status.NoteActive);

        status.SetMicVisible(true);
        Assert.True(status.AsrActive);

        // Stop: the mic is gone but the finalise tail still decodes
        status.SetMicVisible(false);
        status.SetDecodeActive(true);
        Assert.True(status.AsrActive);
        status.SetDecodeActive(false);
        Assert.False(status.AsrActive, "sealed: the dot rests while Averaged shows");

        engine.RaiseNotification("note/partial", Params(new { text = "The" }));
        Assert.True(status.NoteActive);
        engine.RaiseNotification("note/ready");
        Assert.False(status.NoteActive);
    }

    [Fact]
    public async Task MemoryChipShowsTheFootprintAndHidesWhenUnreadable()
    {
        var engine = new FakeEngineClient(autoNotify: false);
        var reading = 5.06;
        var status = new StatusBarViewModel(
            engine, new InlineDispatcher(), memoryGb: () => reading);

        await status.PollMetricsOnceAsync();
        Assert.Equal("Memory · 5.1 GB", status.MemoryChip);

        reading = 0;  // the provider failed; no figure beats a wrong one
        await status.PollMetricsOnceAsync();
        Assert.Equal("", status.MemoryChip);
    }
}
