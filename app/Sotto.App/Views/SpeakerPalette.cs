using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;

namespace Sotto.App.Views;

/// <summary>Speaker roles to their brushes and labels; unlabelled is neutral.</summary>
public static class SpeakerPalette
{
    public static Brush Stripe(string speaker) => speaker switch
    {
        "doctor" => (Brush)Application.Current.Resources["DoctorBrush"],
        "patient" => (Brush)Application.Current.Resources["PatientBrush"],
        _ => (Brush)Application.Current.Resources["TextFillColorTertiaryBrush"],
    };

    public static string Label(string speaker) => speaker switch
    {
        "doctor" => "Doctor",
        "patient" => "Patient",
        "" => "…",
        _ => speaker,
    };
}
