using System.Text.Json;
using Sotto.App.Core.Demo;
using Sotto.App.Core.ViewModels;

namespace Sotto.App.Tests;

public class DemoTrayViewModelTest
{
    private static DemoTrack Track(string name = "Elbow swelling") =>
        new(name, $"C:/demo/{name}.wav");

    [Fact]
    public async Task PlaySendsTheReplayRequest()
    {
        var (session, engine, _) = TestSession.Create();
        var tray = new DemoTrayViewModel(session, [Track()]) { Speed = 4, MonitorAudio = true };

        await tray.PlayCommand.ExecuteAsync(null);

        var start = engine.Requests.Single(r => r.Method == "session/start");
        using var json = JsonDocument.Parse(start.Params);
        var replay = json.RootElement.GetProperty("replay");
        Assert.Equal("C:/demo/Elbow swelling.wav", replay.GetProperty("path").GetString());
        Assert.Equal(4, replay.GetProperty("speed").GetDouble());
        Assert.True(replay.GetProperty("monitor").GetBoolean());
        Assert.Equal(SessionState.Recording, session.State);
    }

    [Fact]
    public void SpeedCyclesAndMarksSmoke()
    {
        var (session, _, _) = TestSession.Create();
        var tray = new DemoTrayViewModel(session, [Track()]);

        Assert.Equal("1×", tray.SpeedLabel);
        Assert.False(tray.IsSmoke);

        tray.CycleSpeedCommand.Execute(null);
        Assert.Equal("4×", tray.SpeedLabel);
        Assert.True(tray.IsSmoke);

        tray.CycleSpeedCommand.Execute(null);
        tray.CycleSpeedCommand.Execute(null);
        tray.CycleSpeedCommand.Execute(null);
        Assert.Equal("1×", tray.SpeedLabel);
    }

    [Fact]
    public async Task PauseTogglesThroughTheEngine()
    {
        var (session, engine, _) = TestSession.Create();
        var tray = new DemoTrayViewModel(session, [Track()]);
        await tray.PlayCommand.ExecuteAsync(null);

        await tray.TogglePauseCommand.ExecuteAsync(null);
        Assert.True(session.Paused);
        await tray.TogglePauseCommand.ExecuteAsync(null);
        Assert.False(session.Paused);

        Assert.Equal(2, engine.Requests.Count(r => r.Method == "session/pause"));
    }

    [Fact]
    public async Task ProgressFollowsDeliveredAudio()
    {
        var (session, engine, _) = TestSession.Create();
        var wav = SessionContractWav.Write(seconds: 2);
        var tray = new DemoTrayViewModel(session, [new DemoTrack("Silence", wav)]);
        await tray.PlayCommand.ExecuteAsync(null);

        for (var i = 0; i < 10; i++)
        {
            engine.RaiseNotification("audio.level", JsonSerializer.SerializeToElement(
                new { level = 0.5, clipped = false }));
        }

        Assert.Equal(0.5, tray.ProgressFraction, 3);
        Assert.Equal("0:01 / 0:02", tray.ProgressText);
    }

    [Fact]
    public async Task PlayIsIdleOnlyAndStopFinalises()
    {
        var (session, _, _) = TestSession.Create();
        var tray = new DemoTrayViewModel(session, [Track()]);

        Assert.True(tray.PlayCommand.CanExecute(null));
        await tray.PlayCommand.ExecuteAsync(null);
        Assert.False(tray.PlayCommand.CanExecute(null));
        Assert.True(tray.StopCommand.CanExecute(null));

        await tray.StopCommand.ExecuteAsync(null);
        Assert.Equal(SessionState.Finalising, session.State);
    }

    [Fact]
    public void BrowseAddsASelectableTrack()
    {
        var (session, _, _) = TestSession.Create();
        var tray = new DemoTrayViewModel(session, [Track()]);

        tray.UseTrack("C:/elsewhere/my_recording.wav");

        Assert.Equal(2, tray.Tracks.Count);
        Assert.Equal("my_recording", tray.SelectedTrack!.Name);
    }
}
