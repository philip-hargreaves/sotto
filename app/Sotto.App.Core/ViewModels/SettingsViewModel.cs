using CommunityToolkit.Mvvm.ComponentModel;
using Sotto.App.Core.Hosting;

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
    private bool _reverting;

    public SettingsViewModel(AppPreferences? preferences = null, IEngineHost? engine = null,
        ISessionState? session = null, StatusBarViewModel? status = null)
    {
        _preferences = preferences;
        _engine = engine;
        _session = session;
        _status = status;
        DemoTrayEnabled = preferences?.DemoTrayEnabled ?? false;
        NpuTranscription = preferences?.NpuTranscription ?? false;
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
