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
    public async Task MicPickerAndNewConsultationNeverShareTheHeaderCell()
    {
        var (session, engine, _) = TestSession.Create();
        var controls = new SessionControlsViewModel(session);

        Assert.True(controls.MicPickerVisible);
        Assert.True(controls.MicPickerEnabled);
        Assert.False(controls.ReviewVisible);

        await session.StartRecordingAsync();
        Assert.True(controls.MicPickerVisible, "shown while recording, read-only");
        Assert.False(controls.MicPickerEnabled, "pinned: changes apply next time");

        await session.StopRecordingAsync();
        engine.RaiseNotification("note/ready");
        Assert.True(controls.ReviewVisible);
        Assert.False(controls.MicPickerVisible, "the cell is New consultation's now");
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

        // Sealed, note not yet streaming: the centre holds and says why
        await controls.StopRecordingCommand.ExecuteAsync(null);
        Assert.True(controls.CentreStageVisible);
        Assert.True(controls.FinalisingVisible);
        Assert.False(controls.PanesVisible);
        Assert.Equal("Preparing note", controls.FinalisingLabel);

        // The first token opens the panes, with the note already filling
        engine.RaiseNotification("note/partial", Params(new { text = "The" }));
        Assert.True(controls.PanesVisible);
        Assert.False(controls.FinalisingVisible);
        engine.RaiseNotification("note/ready");
        Assert.True(controls.ReviewVisible);
        Assert.False(controls.RecordingVisible);
        Assert.False(controls.CentreStageVisible);
    }

    [Fact]
    public async Task AThinRecordingOpensThePanesOnTheCannedNote()
    {
        // No partial ever streams for a thin recording; note/ready must open
        // the panes on its own, or the centre would spin forever
        var (session, engine, _) = TestSession.Create();
        var controls = new SessionControlsViewModel(session);
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        Assert.True(controls.CentreStageVisible);

        engine.RaiseNotification("note/ready", Params(new { text = "The recording was too short" }));

        Assert.True(controls.PanesVisible);
        Assert.True(controls.ReviewVisible);
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
    public async Task FinaliseStagesNameTheCentreSpinner()
    {
        var (session, engine, _) = TestSession.Create();
        var controls = new SessionControlsViewModel(session);
        var phases = new List<FinalisePhase>();
        session.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(ConsultationViewModel.Phase))
            {
                phases.Add(session.Phase);
            }
        };

        await session.StartRecordingAsync();
        engine.RaiseNotification("session/progress", Params(new { stage = "speakers" }));
        Assert.Equal(FinalisePhase.None, session.Phase);  // not finalising yet: ignored

        // The engine reports its stages while session/stop blocks
        engine.BeforeReply = method =>
        {
            if (method != "session/stop")
            {
                return;
            }

            Assert.Equal("Finalising", controls.FinalisingLabel);
            engine.RaiseNotification("session/progress", Params(new { stage = "transcript" }));
            Assert.Equal("Writing transcript", controls.FinalisingLabel);
            engine.RaiseNotification("session/progress", Params(new { stage = "unknown" }));
            Assert.Equal("Writing transcript", controls.FinalisingLabel);  // unknown: unchanged
            engine.RaiseNotification("session/progress", Params(new { stage = "speakers" }));
            Assert.Equal("Labelling speakers", controls.FinalisingLabel);
            // The per-turn re-decode is transcript work again - and on the
            // NPU the longest stage, so the spinner says what it is doing
            engine.RaiseNotification("session/progress", Params(new { stage = "turns" }));
            Assert.Equal("Writing transcript", controls.FinalisingLabel);
        };
        await session.StopRecordingAsync();
        engine.BeforeReply = null;

        // Sealed: the caption names the prefill, and a late stage cannot go back
        Assert.Equal(FinalisePhase.Note, session.Phase);
        Assert.Equal("Preparing note", controls.FinalisingLabel);
        engine.RaiseNotification("session/progress", Params(new { stage = "transcript" }));
        Assert.Equal(FinalisePhase.Note, session.Phase);
        // Start resets to None; the stop then walks forward only
        Assert.Collection(phases.SkipWhile(p => p == FinalisePhase.None),
            p => Assert.Equal(FinalisePhase.Sealing, p),
            p => Assert.Equal(FinalisePhase.Transcript, p),
            p => Assert.Equal(FinalisePhase.Speakers, p),
            p => Assert.Equal(FinalisePhase.Turns, p),
            p => Assert.Equal(FinalisePhase.Note, p));

        // The next stop starts from the beginning again
        engine.RaiseNotification("note/partial", Params(new { text = "The" }));
        Assert.Equal(FinalisePhase.Streaming, session.Phase);
        engine.RaiseNotification("note/ready", Params(new { text = "note" }));
        engine.RaiseNotification("patient/ready", Params(new { text = "sheet" }));
        session.StartNewConsultation();
        Assert.Equal(FinalisePhase.None, session.Phase);
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        Assert.Equal(FinalisePhase.Note, session.Phase);
    }

    private static System.Text.Json.JsonElement Params(object value) =>
        System.Text.Json.JsonSerializer.SerializeToElement(value);

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
    public async Task AResumeThatLosesTheEngineStaysRecordingForTheNextReconnect()
    {
        var (session, engine, _) = TestSession.Create();
        await session.StartRecordingAsync();

        // The double crash seen in the field: the engine dies again while
        // the resume is in flight, and the next reconnect must retry it
        engine.FailNext = method => method == "session/start"
            ? new IOException("pipe transport is closed") : null;
        engine.SetConnected(false);
        engine.SetConnected(true);
        Assert.Equal(SessionState.Recording, session.State);
        Assert.Equal("Recovering", session.Status.LatestActivity);

        engine.SetConnected(false);
        engine.SetConnected(true);

        Assert.Equal(3, engine.Requests.Count(r => r.Method == "session/start"));
        Assert.Equal(SessionState.Recording, session.State);
        Assert.Equal("Recording", session.Status.LatestActivity);
    }

    [Theory]
    [InlineData("efficiency", true)]
    [InlineData("balanced", false)]
    public async Task RecordingWarnsWhenTheMachineIsSavingPower(string mode, bool onMains)
    {
        var (session, _, _) = TestSession.Create(
            powerState: () => new Sotto.App.Core.Metrics.PowerState(mode, onMains));
        await session.StartRecordingAsync();

        Assert.Equal("Recording - saving power, notes will be slower",
            session.Status.LatestActivity);
    }

    [Fact]
    public async Task RecordingOnMainsPowerCarriesNoWarning()
    {
        var (session, _, _) = TestSession.Create(
            powerState: () => new Sotto.App.Core.Metrics.PowerState("performance", true));
        await session.StartRecordingAsync();

        Assert.Equal("Recording", session.Status.LatestActivity);
    }

    [Fact]
    public async Task AResumeTheEngineRefusesKeepsTheSessionAndStopsRecording()
    {
        var (session, engine, _) = TestSession.Create();
        await session.StartRecordingAsync();

        engine.FailNext = method => method == "session/start"
            ? new Sotto.Client.EngineErrorException(-32000, "no stored audio", null) : null;
        engine.SetConnected(false);
        engine.SetConnected(true);

        Assert.Equal(SessionState.Idle, session.State);
        Assert.Equal("Could not resume - session kept", session.Status.LatestActivity);
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
