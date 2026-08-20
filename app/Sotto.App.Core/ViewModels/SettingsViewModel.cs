using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Sotto.App.Core.Hosting;
using Sotto.App.Core.Metrics;

namespace Sotto.App.Core.ViewModels;

/// <summary>
/// Skeleton. Real settings arrive with the features that own them: model tiers
/// with distribution, privacy with storage, performance reporting with telemetry.
/// </summary>
public sealed partial class SettingsViewModel : ObservableObject
{
    private readonly AppPreferences? _preferences;
    private readonly IEngineHost? _engine;
    private readonly ISessionState? _session;
    private readonly StatusBarViewModel? _status;
    private readonly IMachineInfoProvider? _machine;
    private readonly PerformanceCollector? _metrics;
    private readonly string _exportDirectory;
    private bool _reverting;

    public SettingsViewModel(AppPreferences? preferences = null, IEngineHost? engine = null,
        ISessionState? session = null, StatusBarViewModel? status = null,
        IMachineInfoProvider? machine = null, PerformanceCollector? metrics = null,
        string? exportDirectory = null)
    {
        _preferences = preferences;
        _engine = engine;
        _session = session;
        _status = status;
        _machine = machine;
        _metrics = metrics;
        _exportDirectory = exportDirectory
            ?? Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
        DemoTrayEnabled = preferences?.DemoTrayEnabled ?? false;
        NpuTranscription = preferences?.NpuTranscription ?? false;
        CollectPerformanceData = preferences?.CollectPerformanceData ?? false;
    }

    [ObservableProperty]
    public partial string Heading { get; set; } = "Settings";

    /// <summary>Shows the replay tray. A developer control, never clinical.</summary>
    [ObservableProperty]
    public partial bool DemoTrayEnabled { get; set; }

    partial void OnDemoTrayEnabledChanged(bool value)
    {
        if (_preferences is not null)
        {
            _preferences.DemoTrayEnabled = value;
            _preferences.Save();
        }
    }

    /// <summary>Local performance collection; numbers and device names only.</summary>
    [ObservableProperty]
    public partial bool CollectPerformanceData { get; set; }

    partial void OnCollectPerformanceDataChanged(bool value)
    {
        if (_preferences is not null)
        {
            _preferences.CollectPerformanceData = value;
            _preferences.Save();
        }
    }

    [ObservableProperty]
    public partial string ExportResult { get; private set; } = "";

    /// <summary>One self-contained HTML file: readable, emailable, parseable.</summary>
    [RelayCommand]
    private void ExportPerformanceReport()
    {
        if (_machine is null || _metrics is null || !File.Exists(_metrics.Path))
        {
            ExportResult = "no performance data collected yet";
            return;
        }

        try
        {
            var html = ReportBuilder.Build(
                _machine.Describe(), File.ReadAllLines(_metrics.Path), DateTimeOffset.UtcNow);
            var path = Path.Combine(_exportDirectory,
                $"sotto-perf-{Environment.MachineName}-{DateTime.Now:yyyyMMdd}.html");
            File.WriteAllText(path, html);
            ExportResult = $"saved {path}";
        }
        catch (Exception e)
        {
            ExportResult = $"export failed: {e.Message}";
        }
    }

    /// <summary>Runs transcription on the NPU; the engine restarts to apply.</summary>
    [ObservableProperty]
    public partial bool NpuTranscription { get; set; }

    partial void OnNpuTranscriptionChanged(bool value)
    {
        if (_reverting)
        {
            return;
        }

        // A restart would kill a live session
        if (_session?.ConsultationActive == true)
        {
            _reverting = true;
            NpuTranscription = !value;
            _reverting = false;
            _status?.Append("finish the consultation before switching transcription device");
            return;
        }

        if (_preferences is not null)
        {
            _preferences.NpuTranscription = value;
            _preferences.Save();
        }

        if (_engine is not null)
        {
            _status?.Append(value
                ? "switching transcription to the NPU - the first switch can take a few minutes"
                : "switching transcription to the GPU");
            _engine.Shutdown();
            _engine.Start();
        }
    }
}
