using CommunityToolkit.Mvvm.ComponentModel;

namespace Sotto.App.Core.ViewModels;

/// <summary>
/// Skeleton. Real settings arrive with the features that own them: model tiers
/// with distribution, privacy with storage, performance reporting with telemetry.
/// </summary>
public sealed partial class SettingsViewModel : ObservableObject
{
    [ObservableProperty]
    public partial string Heading { get; set; } = "Settings";
}
