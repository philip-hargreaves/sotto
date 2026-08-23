using Sotto.App.Core.ViewModels;

namespace Sotto.App.Tests;

public class SessionCommandsTest
{
    [Fact]
    public async Task CanExecuteFollowsTheSessionState()
    {
        var (session, engine, _) = TestSession.Create();
        var controls = new SessionControlsViewModel(session);

        Assert.True(controls.StartRecordingCommand.CanExecute(null));
        Assert.False(controls.StopRecordingCommand.CanExecute(null));
        Assert.False(controls.NewConsultationCommand.CanExecute(null));

        await session.StartRecordingAsync();
        Assert.False(controls.StartRecordingCommand.CanExecute(null));
        Assert.True(controls.StopRecordingCommand.CanExecute(null));
        Assert.True(controls.CancelRecordingCommand.CanExecute(null));

        await session.StopRecordingAsync();
        engine.RaiseNotification("note/ready");
        Assert.True(controls.NewConsultationCommand.CanExecute(null));
    }

    [Fact]
    public void RecordingWaitsForTheEngine()
    {
        var (session, engine, _) = TestSession.Create();
        var controls = new SessionControlsViewModel(session);

        engine.SetConnected(false);
        Assert.False(controls.StartRecordingCommand.CanExecute(null));

        engine.SetConnected(true);
        Assert.True(controls.StartRecordingCommand.CanExecute(null));
    }

    [Fact]
    public async Task StatePropertyRaisesChangeNotification()
    {
        var (session, _, _) = TestSession.Create();
        var controls = new SessionControlsViewModel(session);
        var raised = false;
        controls.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(SessionControlsViewModel.State))
            {
                raised = true;
            }
        };

        await session.StartRecordingAsync();

        Assert.True(raised);
        Assert.Equal(SessionState.Recording, controls.State);
    }

    [Fact]
    public async Task CommandsDriveTheMachine()
    {
        var (session, engine, _) = TestSession.Create();
        var controls = new SessionControlsViewModel(session);

        await controls.StartRecordingCommand.ExecuteAsync(null);
        Assert.Equal(SessionState.Recording, session.State);

        await controls.StopRecordingCommand.ExecuteAsync(null);
        engine.RaiseNotification("note/ready");
        controls.NewConsultationCommand.Execute(null);

        Assert.Equal(SessionState.Idle, session.State);
    }

    [Fact]
    public async Task VisibilityFollowsTheState()
    {
        var (session, engine, _) = TestSession.Create();
        var controls = new SessionControlsViewModel(session);

        Assert.True(controls.IdleVisible);
        Assert.True(controls.CentreStageVisible);
        Assert.False(controls.PanesVisible);

        await controls.StartRecordingCommand.ExecuteAsync(null);
        Assert.False(controls.IdleVisible);
        Assert.True(controls.RecordingVisible);
        Assert.True(controls.CentreStageVisible);

        await controls.StopRecordingCommand.ExecuteAsync(null);
        Assert.True(controls.PanesVisible);
        Assert.False(controls.FinalisingVisible);
        engine.RaiseNotification("note/ready");
        Assert.True(controls.ReviewVisible);
        Assert.False(controls.RecordingVisible);
        Assert.False(controls.CentreStageVisible);
    }

    [Fact]
    public async Task TheClockFormatsDeliveredAudio()
    {
        var (session, engine, _) = TestSession.Create();
        var controls = new SessionControlsViewModel(session);
        await session.StartRecordingAsync();

        for (var i = 0; i < 754; i++)
        {
            engine.RaiseNotification("audio.level", System.Text.Json.JsonSerializer
                .SerializeToElement(new { level = 0.5, clipped = false }));
        }

        Assert.Equal("01:15", controls.ElapsedLabel);
    }

    [Fact]
    public async Task FirstTimeSetupBlocksRecordingUntilModelsCompile()
    {
        var engine = new FakeEngineClient(autoNotify: false) { FirstUse = true, ModelsCompiled = false };
        var bar = new StatusBarViewModel();
        var session = new ConsultationViewModel(
            engine, new InlineDispatcher(), new TranscriptViewModel(), new NoteViewModel(),
            bar, readinessPollInterval: TimeSpan.FromMilliseconds(1));
        var controls = new SessionControlsViewModel(session);
        bar.SetEngineState(Sotto.App.Core.Hosting.EngineStatus.Running, null);
        bar.SetEngineReady(true);

        Assert.False(session.ModelsReady);
        Assert.False(controls.StartRecordingCommand.CanExecute(null));
        Assert.Contains("First-time setup", bar.DisplayLabel);

        engine.ModelsCompiled = true;
        var deadline = DateTime.UtcNow.AddSeconds(5);
        while (!session.ModelsReady && DateTime.UtcNow < deadline)
        {
            await Task.Delay(10);
        }

        Assert.True(session.ModelsReady);
        Assert.True(controls.StartRecordingCommand.CanExecute(null));
    }

    [Fact]
    public void AWarmLaunchIsNeverGated()
    {
        var (session, engine, _) = TestSession.Create();

        Assert.True(session.ModelsReady);
        Assert.Equal(1, engine.Requests.Count(r => r.Method == "engine/readiness"));
    }

    [Fact]
    public async Task ARestartedEngineResumesTheLiveSession()
    {
        var (session, engine, _) = TestSession.Create();
        await session.StartRecordingAsync();

        engine.SetConnected(false);
        engine.SetConnected(true);

        var starts = engine.Requests.Where(r => r.Method == "session/start").ToList();
        Assert.Equal(2, starts.Count);
        Assert.Contains("resume", starts[1].Params);
        Assert.Contains("s1", starts[1].Params);
        Assert.Equal(SessionState.Recording, session.State);
    }

    [Fact]
    public async Task ARestartWhileIdleDoesNotResume()
    {
        var (session, engine, _) = TestSession.Create();

        engine.SetConnected(false);
        engine.SetConnected(true);

        Assert.DoesNotContain(engine.Requests, r => r.Method == "session/start");
        Assert.Equal(SessionState.Idle, session.State);
    }
}
