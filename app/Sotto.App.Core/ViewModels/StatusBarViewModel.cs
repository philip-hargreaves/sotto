using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using Sotto.App.Core.Hosting;

namespace Sotto.App.Core.ViewModels;

public sealed partial class StatusBarViewModel : ObservableObject
{
    private const int MaxLogEntries = 200;

    [ObservableProperty]
    public partial string EngineStateLabel { get; set; } = "engine: not connected";

    [ObservableProperty]
    public partial string LatestActivity { get; private set; } = "";

    [ObservableProperty]
    public partial string PerformanceLine { get; set; } = "";

    [ObservableProperty]
    public partial double MicLevel { get; private set; }

    [ObservableProperty]
    public partial bool MicClipped { get; private set; }

    /// <summary>The level bar shows only while audio is flowing.</summary>
    [ObservableProperty]
    public partial bool MicVisible { get; private set; }

    public ObservableCollection<string> LogEntries { get; } = [];

    public void SetMicLevel(double level, bool clipped)
    {
        MicLevel = level;
        MicClipped = clipped;
    }

    public void SetMicVisible(bool visible)
    {
        MicVisible = visible;
        if (!visible)
        {
            SetMicLevel(0, false);
        }
    }

    private EngineStatus _status = EngineStatus.Stopped;
    private EngineFault? _fault;
    private bool _ready;

    /// <summary>True while the engine process runs but has not answered yet.</summary>
    [ObservableProperty]
    public partial bool EngineStarting { get; private set; }

    public void SetEngineState(EngineStatus status, EngineFault? fault)
    {
        _status = status;
        _fault = fault;
        Recompute();

        // Silent restarts stay out of the activity log; faults go in
        if (status == EngineStatus.Faulted)
        {
            Append(EngineStateLabel);
        }
    }

    public void SetEngineReady(bool ready)
    {
        _ready = ready;
        Recompute();
    }

    private void Recompute()
    {
        EngineStarting = _status == EngineStatus.Running && !_ready;
        EngineStateLabel = _status switch
        {
            EngineStatus.Running when !_ready => "engine: starting models...",
            EngineStatus.Running => "engine: ready",
            EngineStatus.Restarting => "engine: restarting",
            EngineStatus.Faulted => _fault?.Kind switch
            {
                EngineFaultKind.SessionInterrupted => "engine: crashed mid-consultation",
                EngineFaultKind.LaunchFailed => "engine: unavailable (failed to start)",
                _ => "engine: unavailable (crashing repeatedly)",
            },
            _ => "engine: stopped",
        };
    }

    public void Append(string line)
    {
        LogEntries.Add(line);
        while (LogEntries.Count > MaxLogEntries)
        {
            LogEntries.RemoveAt(0);
        }

        LatestActivity = line;
    }
}
