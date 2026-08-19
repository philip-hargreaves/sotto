using Sotto.App.Core.Hosting;
using Sotto.App.Core.ViewModels;

namespace Sotto.App.Tests;

public class EngineStatusInShellTest
{
    [Fact]
    public void LabelsFollowTheEngineStatusAndReadiness()
    {
        var bar = new StatusBarViewModel();

        bar.SetEngineState(EngineStatus.Running, null);
        Assert.Equal("engine: starting models...", bar.EngineStateLabel);
        Assert.True(bar.EngineStarting);

        bar.SetEngineReady(true);
        Assert.Equal("engine: ready", bar.EngineStateLabel);
        Assert.False(bar.EngineStarting);

        bar.SetEngineReady(false);
        bar.SetEngineState(EngineStatus.Restarting, null);
        Assert.Equal("engine: restarting", bar.EngineStateLabel);

        bar.SetEngineState(EngineStatus.Stopped, null);
        Assert.Equal("engine: stopped", bar.EngineStateLabel);
    }

    [Fact]
    public void FaultsAreLabelledByKindAndLogged()
    {
        var bar = new StatusBarViewModel();

        bar.SetEngineState(
            EngineStatus.Faulted, new EngineFault(EngineFaultKind.SessionInterrupted, -1));
        Assert.Equal("engine: crashed mid-consultation", bar.EngineStateLabel);
        Assert.Equal("engine: crashed mid-consultation", bar.LatestActivity);

        bar.SetEngineState(EngineStatus.Faulted, new EngineFault(EngineFaultKind.CrashLoop, -1));
        Assert.Equal("engine: unavailable (crashing repeatedly)", bar.EngineStateLabel);

        bar.SetEngineState(EngineStatus.Faulted, new EngineFault(EngineFaultKind.LaunchFailed));
        Assert.Equal("engine: unavailable (failed to start)", bar.EngineStateLabel);

        Assert.Equal(3, bar.LogEntries.Count);
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
