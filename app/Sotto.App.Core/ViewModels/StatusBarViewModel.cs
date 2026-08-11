using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;

namespace Sotto.App.Core.ViewModels;

public sealed partial class StatusBarViewModel : ObservableObject
{
    private const int MaxLogEntries = 200;

    [ObservableProperty]
    public partial string EngineStateLabel { get; set; } = "engine: not connected";

    [ObservableProperty]
    public partial string PerformanceLine { get; set; } = "";

    public ObservableCollection<string> LogEntries { get; } = [];

    public void Append(string line)
    {
        LogEntries.Add(line);
        while (LogEntries.Count > MaxLogEntries)
        {
            LogEntries.RemoveAt(0);
        }
    }
}
