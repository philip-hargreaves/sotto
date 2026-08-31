using System.Text.Json;

namespace Ambient.App.Core;

/// <summary>One small json file of app preferences; absent means defaults.</summary>
public sealed class AppPreferences(string path)
{
    private sealed record Stored(
        bool DemoTrayEnabled, bool NpuTranscription, bool CollectPerformanceData,
        string? NoteStyle = null, string? NoteDetail = null,
        bool KeepConsultations = false, bool ShowPerformanceMetrics = false,
        string? MicId = null, string? Theme = null);

    public bool DemoTrayEnabled { get; set; }

    public bool NpuTranscription { get; set; }

    public bool CollectPerformanceData { get; set; }

    /// <summary>
    /// Off by default: the app saves nothing beyond the consultation unless
    /// the clinician opts in. Applies to consultations from now on.
    /// </summary>
    public bool KeepConsultations { get; set; }

    /// <summary>Off by default: the status-bar chips are for testing, not GPs.</summary>
    public bool ShowPerformanceMetrics { get; set; }

    /// <summary>The chosen microphone's endpoint id; empty means the default.</summary>
    public string MicId { get; set; } = "";

    /// <summary>"system" follows the OS; "light" and "dark" override it.</summary>
    public string Theme { get; set; } = "system";

    public string NoteStyle { get; set; } = "prose";

    public string NoteDetail { get; set; } = "standard";

    public static AppPreferences Load(string path)
    {
        var preferences = new AppPreferences(path);
        try
        {
            var stored = JsonSerializer.Deserialize<Stored>(File.ReadAllText(path));
            preferences.DemoTrayEnabled = stored?.DemoTrayEnabled ?? false;
            preferences.NpuTranscription = stored?.NpuTranscription ?? false;
            preferences.CollectPerformanceData = stored?.CollectPerformanceData ?? false;
            preferences.KeepConsultations = stored?.KeepConsultations ?? false;
            preferences.ShowPerformanceMetrics = stored?.ShowPerformanceMetrics ?? false;
            preferences.MicId = stored?.MicId ?? "";
            // Values the shell cannot render never leave this boundary
            preferences.Theme = stored?.Theme is "light" or "dark" ? stored.Theme : "system";
            // Values the engine would refuse never leave this boundary
            preferences.NoteStyle = stored?.NoteStyle is "prose" or "soap"
                ? stored.NoteStyle : "prose";
            preferences.NoteDetail = stored?.NoteDetail is "concise" or "standard" or "detailed"
                ? stored.NoteDetail : "standard";
        }
        catch (Exception)
        {
        }

        return preferences;
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, JsonSerializer.Serialize(new Stored(
                DemoTrayEnabled, NpuTranscription, CollectPerformanceData,
                NoteStyle, NoteDetail, KeepConsultations, ShowPerformanceMetrics, MicId,
                Theme)));
        }
        catch (Exception)
        {
        }
    }
}
