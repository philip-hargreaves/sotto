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
}
