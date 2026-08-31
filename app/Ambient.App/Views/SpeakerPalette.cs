using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;

namespace Ambient.App.Views;

/// <summary>Speaker roles to brushes and labels, resolved by the theme in
/// effect - a plain resource lookup follows the OS theme, not the chosen one.
/// The consuming view keeps <see cref="Theme"/> current.</summary>
public static class SpeakerPalette
{
    public static ElementTheme Theme { get; set; } = ElementTheme.Dark;

    public static Brush Stripe(string speaker) => Themed(speaker switch
    {
        "doctor" => "DoctorBrush",
        "patient" => "PatientBrush",
        _ => "UnknownSpeakerBrush",
    });

    public static string Label(string speaker) => speaker switch
    {
        "doctor" => "Doctor",
        "patient" => "Patient",
        "" => "…",
        _ => speaker,
    };

    private static Brush Themed(string key)
    {
        // Dark is keyed "Default" in the dictionaries, as WinUI expects
        var theme = Theme == ElementTheme.Light ? "Light" : "Default";
        if (Find(Application.Current.Resources, theme, key) is Brush brush)
        {
            return brush;
        }

        return (Brush)Application.Current.Resources[key];  // high contrast et al
    }

    private static object? Find(ResourceDictionary dictionary, string theme, string key)
    {
        if (dictionary.ThemeDictionaries.TryGetValue(theme, out var themed)
            && themed is ResourceDictionary resolved
            && resolved.TryGetValue(key, out var direct))
        {
            return direct;
        }

        foreach (var merged in dictionary.MergedDictionaries)
        {
            if (Find(merged, theme, key) is { } inherited)
            {
                return inherited;
            }
        }

        return null;
    }
}
