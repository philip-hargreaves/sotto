using System.Text.Json;
using Sotto.App.Core.ViewModels;
using Sotto.Client;

namespace Sotto.App.Tests;

public class SessionStateMachineTest
{
    private sealed class TimingOutClient : IEngineClient
    {
        public event Action<string, JsonElement>? NotificationReceived
        {
            add { }
            remove { }
        }

        public event Action<bool>? ConnectedChanged
        {
            add { }
            remove { }
        }

        public bool Connected => true;

        public Task<JsonElement> RequestAsync(
            string method, object? parameters, TimeSpan timeout,
            CancellationToken cancellationToken = default) =>
            method == "session/stop"
                ? Task.FromException<JsonElement>(new TaskCanceledException())
                : Task.FromResult(JsonSerializer.SerializeToElement(new { }));

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }

    private sealed class RefusingClient : IEngineClient
    {
        public event Action<string, JsonElement>? NotificationReceived
        {
            add { }
            remove { }
        }

        public event Action<bool>? ConnectedChanged
        {
            add { }
            remove { }
        }

        public bool Connected => true;

        public Task<JsonElement> RequestAsync(
            string method, object? parameters, TimeSpan timeout,
            CancellationToken cancellationToken = default) =>
            Task.FromException<JsonElement>(
                new InvalidOperationException("the speech model is still loading"));

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }

    [Fact]
    public async Task AFailedStartStaysIdleWithTheReasonLogged()
    {
        var status = new StatusBarViewModel();
        var session = new ConsultationViewModel(
            new RefusingClient(), new InlineDispatcher(), new TranscriptViewModel(),
            new NoteViewModel(), status);

        await session.StartRecordingAsync();

        Assert.Equal(SessionState.Idle, session.State);
        Assert.Contains(status.LogEntries, line => line.Contains("still loading"));
    }

    [Fact]
    public async Task ATimedOutStopRecoversToIdle()
    {
        var status = new StatusBarViewModel();
        var session = new ConsultationViewModel(
            new TimingOutClient(), new InlineDispatcher(), new TranscriptViewModel(),
            new NoteViewModel(), status);
        await session.StartRecordingAsync();

        await session.StopRecordingAsync();

        Assert.Contains(status.LogEntries, line => line.Contains("Taking longer"));
        Assert.Contains(status.LogEntries, line => line.Contains("Stop failed"));
        Assert.Equal(SessionState.Idle, session.State);  // never wedged in Finalising
    }

    [Fact]
    public async Task FullLifecycleAdvancesThroughEveryState()
    {
        var (session, engine, _) = TestSession.Create();
        Assert.Equal(SessionState.Idle, session.State);

        await session.StartRecordingAsync();
        Assert.Equal(SessionState.Recording, session.State);

        await session.StopRecordingAsync();
        Assert.Equal(SessionState.Finalising, session.State);

        engine.RaiseNotification("note/ready");
        Assert.Equal(SessionState.Review, session.State);

        session.StartNewConsultation();
        Assert.Equal(SessionState.Idle, session.State);
    }

    [Fact]
    public async Task CancelReturnsFromRecordingToIdle()
    {
        var (session, _, _) = TestSession.Create();
        await session.StartRecordingAsync();

        await session.CancelRecordingAsync();

        Assert.Equal(SessionState.Idle, session.State);
    }

    [Fact]
    public async Task IllegalTransitionsAreRefused()
    {
        var (session, engine, _) = TestSession.Create();

        // Nothing but start is legal from idle
        await session.StopRecordingAsync();
        await session.CancelRecordingAsync();
        session.StartNewConsultation();
        Assert.Equal(SessionState.Idle, session.State);

        // A note/ready that arrives outside finalising must not move the state
        engine.RaiseNotification("note/ready");
        Assert.Equal(SessionState.Idle, session.State);

        await session.StartRecordingAsync();
        await session.StartRecordingAsync();
        Assert.Equal(SessionState.Recording, session.State);

        session.StartNewConsultation();
        Assert.Equal(SessionState.Recording, session.State);
    }

    [Fact]
    public async Task UnknownNotificationsAreIgnored()
    {
        var (session, engine, _) = TestSession.Create();
        await session.StartRecordingAsync();

        engine.RaiseNotification("engine/unheard-of");

        Assert.Equal(SessionState.Recording, session.State);
    }
}
