using Ambient.App.Core.Hosting;
using Ambient.App.Core.ViewModels;

namespace Ambient.App.Tests;

public class EngineStatusInShellTest
{
    [Fact]
    public void LabelsFollowTheEngineStatusAndReadiness()
    {
        var bar = new StatusBarViewModel();

        bar.SetEngineState(EngineStatus.Running, null);
        Assert.Equal("Setting up", bar.EngineStateLabel);
        Assert.True(bar.EngineStarting);

        bar.SetEngineReady(true);
        Assert.Equal("Ready", bar.EngineStateLabel);
        Assert.False(bar.EngineStarting);

        bar.SetEngineReady(false);
        bar.SetEngineState(EngineStatus.Restarting, null);
        Assert.Equal("Recovering", bar.EngineStateLabel);

        bar.SetEngineState(EngineStatus.Stopped, null);
        Assert.Equal("Not running", bar.EngineStateLabel);
    }

    [Fact]
    public void FaultsAreLabelledByKindAndLogged()
    {
        var bar = new StatusBarViewModel();

        bar.SetEngineState(
            EngineStatus.Faulted, new EngineFault(EngineFaultKind.SessionInterrupted, -1));
        Assert.Equal("A problem interrupted the consultation - recovering", bar.EngineStateLabel);
        Assert.Equal("A problem interrupted the consultation - recovering", bar.LatestActivity);

        bar.SetEngineState(EngineStatus.Faulted, new EngineFault(EngineFaultKind.CrashLoop, -1));
        Assert.Equal("Recording is unavailable - please restart the app", bar.EngineStateLabel);

        bar.SetEngineState(EngineStatus.Faulted, new EngineFault(EngineFaultKind.LaunchFailed));
        Assert.Equal("Recording is unavailable - please restart the app", bar.EngineStateLabel);

        Assert.Equal(3, bar.LogEntries.Count);
    }

    [Fact]
    public void OneStatusReplacedWithTheRingMeaningInProgress()
    {
        var bar = new StatusBarViewModel();
        bar.SetEngineState(EngineStatus.Running, null);
        bar.SetEngineReady(true);
        Assert.Equal("Ready", bar.DisplayLabel);
        Assert.False(bar.Busy);

        bar.Append("Finalising", busy: true);
        Assert.Equal("Finalising", bar.DisplayLabel);
        Assert.True(bar.Busy);

        bar.Append("Ready for review");
        Assert.Equal("Ready for review", bar.DisplayLabel);
        Assert.False(bar.Busy);

        // Abnormal readiness outranks whatever activity was showing
        bar.SetEngineReady(false);
        bar.SetEngineState(EngineStatus.Restarting, null);
        Assert.Equal("Recovering", bar.DisplayLabel);
        Assert.True(bar.Busy);
    }

    [Fact]
    public void TheMeterRollsHistoryThroughFixedBars()
    {
        var bar = new StatusBarViewModel();
        var first = bar.Meter[0];

        bar.SetMicLevel(1.0, clipped: false);
        Assert.Equal(28, bar.Meter[^1].Height);

        bar.SetMicLevel(0, clipped: false);
        Assert.Equal(28, bar.Meter[^2].Height);
        Assert.Equal(2, bar.Meter[^1].Height);
        Assert.Same(first, bar.Meter[0]);
    }

    [Fact]
    public void SilentTransitionsStayOutOfTheActivityLog()
    {
        var bar = new StatusBarViewModel();

        bar.SetEngineState(EngineStatus.Running, null);
        bar.SetEngineState(EngineStatus.Restarting, null);
        bar.SetEngineState(EngineStatus.Running, null);

        Assert.Empty(bar.LogEntries);
    }

    [Fact]
    public async Task ConsultationActiveTracksTheSessionState()
    {
        var (session, engine, _) = TestSession.Create();
        Assert.False(session.ConsultationActive);

        await session.StartRecordingAsync();
        Assert.True(session.ConsultationActive);

        await session.StopRecordingAsync();
        Assert.True(session.ConsultationActive);

        engine.RaiseNotification("note/ready");
        Assert.True(session.ConsultationActive);

        session.StartNewConsultation();
        Assert.False(session.ConsultationActive);
    }
}
