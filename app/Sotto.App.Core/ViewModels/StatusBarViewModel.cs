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

    public ObservableCollection<string> LogEntries { get; } = [];

    public void SetEngineState(EngineStatus status, EngineFault? fault)
    {
        EngineStateLabel = status switch
        {
            EngineStatus.Running => "engine: running",
            EngineStatus.Restarting => "engine: restarting",
            EngineStatus.Faulted => fault?.Kind switch
            {
                EngineFaultKind.SessionInterrupted => "engine: crashed mid-consultation",
                EngineFaultKind.LaunchFailed => "engine: unavailable (failed to start)",
                _ => "engine: unavailable (crashing repeatedly)",
            },
            _ => "engine: stopped",
        };

        // Silent restarts stay out of the activity log; faults go in
        if (status == EngineStatus.Faulted)
        {
            Append(EngineStateLabel);
        }
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
