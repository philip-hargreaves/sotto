using Sotto.App.Core.ViewModels;

namespace Sotto.App.Tests;

public class SessionStateMachineTest
{
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
