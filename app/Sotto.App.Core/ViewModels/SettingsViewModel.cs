using CommunityToolkit.Mvvm.ComponentModel;

namespace Sotto.App.Core.ViewModels;

/// <summary>
/// Skeleton. Real settings arrive with the features that own them: model tiers
/// with distribution, privacy with storage, performance reporting with telemetry.
/// </summary>
public sealed partial class SettingsViewModel : ObservableObject
{
    private readonly AppPreferences? _preferences;

    public SettingsViewModel(AppPreferences? preferences = null)
    {
        _preferences = preferences;
        DemoTrayEnabled = preferences?.DemoTrayEnabled ?? false;
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
}
