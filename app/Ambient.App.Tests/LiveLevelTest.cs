using System.Text.Json;
using Ambient.App.Core.ViewModels;

namespace Ambient.App.Tests;

public class LiveLevelTest
{
    private static JsonElement Params(object value) => JsonSerializer.SerializeToElement(value);

    [Fact]
    public void LevelNotificationsReachTheStatusBar()
    {
        var (session, engine, _) = TestSession.Create();

        engine.RaiseNotification("audio.level", Params(new { level = 0.5, clipped = true }));

        Assert.Equal(0.5, session.Status.MicLevel);
        Assert.True(session.Status.MicClipped);
    }

    [Fact]
    public async Task InterruptionMidRecordingTellsTheClinicianAndResets()
    {
        var (session, engine, note) = TestSession.Create();
        await session.StartRecordingAsync();
        engine.RaiseNotification("audio.level", Params(new { level = 0.8, clipped = false }));

        engine.RaiseNotification(
            "session/interrupted", Params(new { reason = "deviceLost", detail = "unplugged" }));

        Assert.Equal(SessionState.Idle, session.State);
        Assert.Equal(NotePipelineState.Pending, note.PipelineState);
        Assert.Contains("unplugged", session.Status.LatestActivity);
        Assert.Equal(0, session.Status.MicLevel);
    }

    [Fact]
    public async Task InterruptionDuringFinalisingAlsoResets()
    {
        var (session, engine, note) = TestSession.Create();
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();

        engine.RaiseNotification(
            "session/interrupted", Params(new { reason = "failed", detail = "driver gone" }));

        Assert.Equal(SessionState.Idle, session.State);
        Assert.Equal(NotePipelineState.Pending, note.PipelineState);
    }

    [Fact]
    public void InterruptionWhileIdleIsIgnored()
    {
        var (session, engine, _) = TestSession.Create();

        engine.RaiseNotification(
            "session/interrupted", Params(new { reason = "failed", detail = "stray" }));

        Assert.Equal(SessionState.Idle, session.State);
        Assert.Equal("", session.Status.LatestActivity);
    }
}
