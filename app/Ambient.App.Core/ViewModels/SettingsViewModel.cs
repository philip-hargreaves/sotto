using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Sotto.App.Core.Hosting;
using Sotto.App.Core.Metrics;

namespace Sotto.App.Core.ViewModels;

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
        // Restoring saved values is not the clinician changing them: the
        // handlers (persist, confirm, engine restart) must not fire here
        _initialising = true;
        DemoTrayEnabled = preferences?.DemoTrayEnabled ?? false;
        NpuTranscription = preferences?.NpuTranscription ?? false;
        CollectPerformanceData = preferences?.CollectPerformanceData ?? false;
        KeepConsultations = preferences?.KeepConsultations ?? false;
        ShowPerformanceMetrics = preferences?.ShowPerformanceMetrics ?? false;
        Theme = preferences?.Theme ?? "system";
        _initialising = false;
    }

    private readonly bool _initialising;

    /// <summary>
    /// Off by default: a consultation is erased when it is left. On: the
    /// encrypted history. Applies to consultations from now on; audio is
    /// never kept either way.
    /// </summary>
    [ObservableProperty]
    public partial bool KeepConsultations { get; set; }

    /// <summary>
    /// Turning the history ON starts accumulating patient records, so it is
    /// confirmed, never just toggled; the view supplies the dialog. Off is
    /// frictionless - reducing retention is never gated.
    /// </summary>
    public Func<Task<bool>>? ConfirmKeepConsultations { get; set; }

    partial void OnKeepConsultationsChanged(bool value)
    {
        if (_reverting || _initialising)
        {
            return;
        }

        if (value && ConfirmKeepConsultations is not null)
        {
            _reverting = true;
            KeepConsultations = false;  // holds until the clinician confirms
            _reverting = false;
            _ = AskThenEnableAsync();
            return;
        }

        PersistKeepConsultations(value);
    }

    private async Task AskThenEnableAsync()
    {
        if (await ConfirmKeepConsultations!().ConfigureAwait(true))
        {
            _reverting = true;
            KeepConsultations = true;
            _reverting = false;
            PersistKeepConsultations(true);
        }
    }

    private void PersistKeepConsultations(bool value)
    {
        if (_preferences is not null)
        {
            _preferences.KeepConsultations = value;
            _preferences.Save();
        }
    }

    [ObservableProperty]
    public partial string Heading { get; set; } = "Settings";

    /// <summary>"system", "light" or "dark"; the shell applies it live.</summary>
    [ObservableProperty]
    public partial string Theme { get; set; } = "system";

    /// <summary>Wired by the shell to the window's requested theme.</summary>
    public Action<string>? ApplyTheme { get; set; }

    partial void OnThemeChanged(string value)
    {
        OnPropertyChanged(nameof(ThemeIndex));
        if (_initialising)
        {
            return;
        }

        ApplyTheme?.Invoke(value);
        if (_preferences is not null)
        {
            _preferences.Theme = value;
            _preferences.Save();
        }
    }

    public IReadOnlyList<string> ThemeOptions { get; } = ["System default", "Light", "Dark"];

    /// <summary>Theme as the Appearance control's selection, same order.</summary>
    public int ThemeIndex
    {
        get => Theme switch { "light" => 1, "dark" => 2, _ => 0 };
        set => Theme = value switch { 1 => "light", 2 => "dark", _ => "system" };
    }

    /// <summary>Shows the replay tray. A developer control, never clinical.</summary>
    [ObservableProperty]
    public partial bool DemoTrayEnabled { get; set; }

    partial void OnDemoTrayEnabledChanged(bool value)
    {
        if (!_initialising && _preferences is not null)
        {
            _preferences.DemoTrayEnabled = value;
            _preferences.Save();
        }
    }

    /// <summary>Shows the status-bar model and memory chips. For testing.</summary>
    [ObservableProperty]
    public partial bool ShowPerformanceMetrics { get; set; }

    partial void OnShowPerformanceMetricsChanged(bool value)
    {
        if (_initialising)
        {
            return;
        }

        if (_status is not null)
        {
            _status.MetricsVisible = value;
        }

        if (_preferences is not null)
        {
            _preferences.ShowPerformanceMetrics = value;
            _preferences.Save();
        }
    }

    /// <summary>Local performance collection; numbers and device names only.</summary>
    [ObservableProperty]
    public partial bool CollectPerformanceData { get; set; }

    partial void OnCollectPerformanceDataChanged(bool value)
    {
        if (!_initialising && _preferences is not null)
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
        if (_reverting || _initialising)
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
                : "switching transcription to the GPU", busy: true);
            _engine.Shutdown();
            _engine.Start();
        }
    }
}
