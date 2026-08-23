using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using Sotto.App.Core.Hosting;

namespace Sotto.App.Core.ViewModels;

public sealed partial class StatusBarViewModel : ObservableObject
{
    private const int MaxLogEntries = 200;

    // Clinician-facing: no engine, model or process vocabulary
    [ObservableProperty]
    public partial string EngineStateLabel { get; set; } = "Starting";

    [ObservableProperty]
    public partial string LatestActivity { get; private set; } = "";

    // One status on screen, replaced as things happen: abnormal readiness
    // outranks activity, activity outranks Ready; Busy drives the one ring
    public string DisplayLabel =>
        !_ready || _status != EngineStatus.Running ? EngineStateLabel
        : LatestActivity.Length > 0 ? LatestActivity
        : EngineStateLabel;

    public bool Busy => EngineStarting || _activityBusy;

    private bool _activityBusy;

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

    /// <summary>True in every transient state; the status ring spins on it.</summary>
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
        EngineStarting = _status == EngineStatus.Running && !_ready
            || _status == EngineStatus.Restarting;
        EngineStateLabel = _status switch
        {
            EngineStatus.Running when !_ready => "Setting up",
            EngineStatus.Running => "Ready",
            EngineStatus.Restarting => "Recovering",
            EngineStatus.Faulted => _fault?.Kind switch
            {
                EngineFaultKind.SessionInterrupted =>
                    "A problem interrupted the consultation - recovering",
                _ => "Recording is unavailable - please restart the app",
            },
            _ => "Not running",
        };
        OnPropertyChanged(nameof(DisplayLabel));
        OnPropertyChanged(nameof(Busy));
    }

    /// <summary>Log-only detail; the displayed status stays concise.</summary>
    public void Log(string line)
    {
        LogEntries.Add(line);
        while (LogEntries.Count > MaxLogEntries)
        {
            LogEntries.RemoveAt(0);
        }
    }

    public void Append(string line, bool busy = false)
    {
        LogEntries.Add(line);
        while (LogEntries.Count > MaxLogEntries)
        {
            LogEntries.RemoveAt(0);
        }

        _activityBusy = busy;
        LatestActivity = line;
        OnPropertyChanged(nameof(DisplayLabel));
        OnPropertyChanged(nameof(Busy));
    }
}
