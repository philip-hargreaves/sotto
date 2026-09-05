using System.Collections.ObjectModel;
using System.Text.Json;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Ambient.App.Core.Hosting;
using Ambient.App.Core.Metrics;
using Ambient.Client;

namespace Ambient.App.Core.ViewModels;

public sealed partial class SettingsViewModel : ObservableObject
{
    private static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(30);

    private readonly AppPreferences? _preferences;
    private readonly IEngineHost? _engine;
    private readonly ISessionState? _session;
    private readonly StatusBarViewModel? _status;
    private readonly IMachineInfoProvider? _machine;
    private readonly PerformanceCollector? _metrics;
    private readonly IEngineClient? _client;
    private readonly IUiDispatcher? _dispatcher;
    private readonly string _exportDirectory;
    private bool _reverting;

    public SettingsViewModel(AppPreferences? preferences = null, IEngineHost? engine = null,
        ISessionState? session = null, StatusBarViewModel? status = null,
        IMachineInfoProvider? machine = null, PerformanceCollector? metrics = null,
        string? exportDirectory = null, IEngineClient? client = null,
        IUiDispatcher? dispatcher = null)
    {
        _preferences = preferences;
        _engine = engine;
        _session = session;
        _status = status;
        _machine = machine;
        _metrics = metrics;
        _client = client;
        _dispatcher = dispatcher;
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
        _noteTier = preferences?.NoteTier ?? "default";
        _initialising = false;

        if (client is not null)
        {
            // Options come from the engine's store; the lane's state drives the control
            client.ConnectedChanged += connected => Post(() =>
            {
                if (connected)
                {
                    _ = LoadNoteModelsAsync();
                }
            });
            client.NotificationReceived += (method, parameters) =>
            {
                if (method == "note/model")
                {
                    var snapshot = parameters.Clone();
                    Post(() => OnNoteModel(snapshot));
                }
            };
            if (client.Connected)
            {
                _ = LoadNoteModelsAsync();
            }
        }
    }

    private readonly bool _initialising;

    private void Post(Action action)
    {
        if (_dispatcher is null)
        {
            action();
        }
        else
        {
            _dispatcher.Post(action);
        }
    }

    // ---- note model tier --------------------------------------------------

    // Tier keys in ladder order, parallel to NoteModelOptions
    private readonly List<string> _tiers = [];
    private string _noteTier;
    private string? _revertTier;  // where a failed switch goes back to
    private bool _populating;

    /// <summary>Display names of the staged note models, smallest first.</summary>
    public ObservableCollection<string> NoteModelOptions { get; } = [];

    /// <summary>The chosen note model as the control's selection.</summary>
    [ObservableProperty]
    public partial int NoteModelIndex { get; set; } = -1;

    /// <summary>False while the lane loads, and when there is nothing to choose between.</summary>
    [ObservableProperty]
    public partial bool NoteModelEnabled { get; set; }

    /// <summary>Lane status shown under the control.</summary>
    [ObservableProperty]
    public partial string NoteModelStatus { get; set; } = "";

    /// <summary>The tier the shell wants; the engine's store resolves it.</summary>
    public string NoteTier => _noteTier;

    private static int LadderRank(string tier) => tier switch
    {
        "constrained" => 0,
        "default" => 1,
        "accuracy" => 2,
        _ => 3,
    };

    private async Task LoadNoteModelsAsync()
    {
        if (_client is null || !_client.Connected)
        {
            return;
        }

        try
        {
            var response = await _client
                .RequestAsync("engine/models", null, TimeSpan.FromSeconds(5))
                .ConfigureAwait(true);
            var staged = response.GetProperty("models").EnumerateArray()
                .Where(m => m.GetProperty("task").GetString() == "note")
                .Select(m => (
                    Tier: m.GetProperty("tier").GetString() ?? "",
                    Name: m.TryGetProperty("name", out var n) && !string.IsNullOrWhiteSpace(n.GetString())
                        ? n.GetString()!
                        : StatusBarViewModel.FriendlyModelName(m.GetProperty("id").GetString() ?? "")))
                .Where(m => AppPreferences.NoteTiers.Contains(m.Tier))
                .OrderBy(m => LadderRank(m.Tier))
                .ToList();

            _populating = true;
            // Rebuilt only when the store's contents changed: clearing ComboBox items
            // under an open popup or a live selection can fault in XAML, and every
            // reconnect would otherwise do it
            var tiers = staged.Select(m => m.Tier).ToList();
            var names = staged.Select(m => m.Name).ToList();
            if (!tiers.SequenceEqual(_tiers) || !names.SequenceEqual(NoteModelOptions))
            {
                NoteModelIndex = -1;  // clear the selection before the items
                _tiers.Clear();
                NoteModelOptions.Clear();
                foreach (var (tier, name) in staged)
                {
                    _tiers.Add(tier);
                    NoteModelOptions.Add(name);
                }
            }

            NoteModelStatus = _tiers.Count <= 1 ? "Only one note model is installed" : "";
            // Preference for an unstaged tier: the engine stayed on the default,
            // and the control shows that
            if (!_tiers.Contains(_noteTier) && _tiers.Contains("default"))
            {
                NoteModelStatus = $"The saved choice is not installed; using {NameOf("default")}";
                _noteTier = "default";
                PersistTier();
            }

            NoteModelIndex = _tiers.IndexOf(_noteTier);
            _populating = false;
            NoteModelEnabled = _tiers.Count > 1;
        }
        catch (Exception)
        {
            _populating = false;
        }
    }

    private string NameOf(string tier)
    {
        var index = _tiers.IndexOf(tier);
        return index >= 0 && index < NoteModelOptions.Count ? NoteModelOptions[index] : tier;
    }

    partial void OnNoteModelIndexChanged(int value)
    {
        if (_reverting || _initialising || _populating || value < 0 || value >= _tiers.Count)
        {
            return;
        }

        var tier = _tiers[value];
        if (tier == _noteTier)
        {
            return;
        }

        // The switch ends the resident model; a consultation needs it
        if (_session?.ConsultationActive == true)
        {
            _reverting = true;
            NoteModelIndex = _tiers.IndexOf(_noteTier);
            _reverting = false;
            _status?.Append("finish the consultation before changing the note model");
            return;
        }

        _revertTier = _noteTier;
        _noteTier = tier;
        PersistTier();
        NoteModelEnabled = false;
        NoteModelStatus = $"Loading {NameOf(tier)}";
        _status?.Append($"Loading {NameOf(tier)}", busy: true);
        _ = SendTierAsync(tier);
    }

    private void PersistTier()
    {
        if (_preferences is not null)
        {
            _preferences.NoteTier = _noteTier;
            _preferences.Save();
        }
    }

    private async Task SendTierAsync(string tier)
    {
        if (_client is null)
        {
            return;
        }

        try
        {
            var reply = await _client
                .RequestAsync("note/tier", new { tier }, RequestTimeout)
                .ConfigureAwait(true);
            // Already resident (the same tier after a restart): nothing to wait for
            if (reply.TryGetProperty("state", out var state) && state.GetString() == "ready")
            {
                OnNoteModel(reply);
            }
        }
        catch (Exception e)
        {
            RevertTier(e.Message);
        }
    }

    // A refused or failed switch reverts to the previous tier, once; the
    // engine is told
    private void RevertTier(string reason)
    {
        var back = _revertTier;
        _revertTier = null;
        NoteModelStatus = $"Could not switch: {reason}";
        _status?.Append($"Could not switch note model: {reason}");
        if (back is null)
        {
            NoteModelEnabled = _tiers.Count > 1;
            return;
        }

        _noteTier = back;
        PersistTier();
        _reverting = true;
        NoteModelIndex = _tiers.IndexOf(back);
        _reverting = false;
        _ = SendTierAsync(back);
    }

    private void OnNoteModel(JsonElement parameters)
    {
        if (parameters.ValueKind != JsonValueKind.Object)
        {
            return;
        }

        var state = parameters.TryGetProperty("state", out var s) ? s.GetString() ?? "" : "";
        var tier = parameters.TryGetProperty("tier", out var t) ? t.GetString() ?? "" : "";
        var name = parameters.TryGetProperty("name", out var n) && !string.IsNullOrEmpty(n.GetString())
            ? n.GetString()!
            : NameOf(tier);
        switch (state)
        {
            case "loading":
                NoteModelEnabled = false;
                var firstUse = parameters.TryGetProperty("firstUse", out var f) && f.GetBoolean();
                NoteModelStatus = firstUse
                    ? $"Preparing {name} for this computer - the first time takes a few minutes"
                    : $"Loading {name}";
                break;
            case "ready":
                _revertTier = null;
                NoteModelEnabled = _tiers.Count > 1;
                var seconds = parameters.TryGetProperty("seconds", out var sec) ? sec.GetDouble() : 0;
                NoteModelStatus = seconds > 0
                    ? FormattableString.Invariant($"{name} ready ({seconds:0} s)")
                    : $"{name} ready";
                // The engine is authoritative about what is resident
                if (_tiers.Contains(tier) && tier != _noteTier)
                {
                    _noteTier = tier;
                    PersistTier();
                    _reverting = true;
                    NoteModelIndex = _tiers.IndexOf(tier);
                    _reverting = false;
                }

                break;
            case "failed":
                var detail = parameters.TryGetProperty("detail", out var d) ? d.GetString() ?? "" : "";
                if (tier == _noteTier)
                {
                    RevertTier(detail);
                }
                else
                {
                    NoteModelStatus = $"{name} failed: {detail}";
                }

                break;
            default:
                break;
        }
    }

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

    /// <summary>Suggested name in, chosen path (or null) out; the view owns the picker.</summary>
    public Func<string, Task<string?>>? PickSavePath { get; set; }

    /// <summary>One self-contained HTML file: readable, emailable, parseable.</summary>
    [RelayCommand]
    private async Task ExportPerformanceReport()
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
            var suggested = $"ambient-perf-{Environment.MachineName}-{DateTime.Now:yyyyMMdd}.html";
            var path = PickSavePath is not null
                ? await PickSavePath(suggested).ConfigureAwait(true)
                : Path.Combine(_exportDirectory, suggested);
            if (path is null)
            {
                return;  // cancelled: no file, no caption
            }

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
                ? "preparing the low-power model - the first switch can take a few minutes"
                : "switching speech recognition to the GPU", busy: true);
            _engine.Shutdown();
            _engine.Start();
        }
    }
}
