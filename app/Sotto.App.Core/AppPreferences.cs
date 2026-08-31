using System.Text.Json;

namespace Sotto.App.Core;

/// <summary>One small json file of app preferences; absent means defaults.</summary>
public sealed class AppPreferences(string path)
{
    private sealed record Stored(
        bool DemoTrayEnabled, bool NpuTranscription, bool CollectPerformanceData,
        string? NoteStyle = null, string? NoteDetail = null,
        bool KeepConsultations = false);

    public bool DemoTrayEnabled { get; set; }

    public bool NpuTranscription { get; set; }

    public bool CollectPerformanceData { get; set; }

    /// <summary>
    /// Off by default: the app saves nothing beyond the consultation unless
    /// the clinician opts in. Applies to consultations from now on.
    /// </summary>
    public bool KeepConsultations { get; set; }

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
                NoteStyle, NoteDetail, KeepConsultations)));
        }
        catch (Exception)
        {
        }
    }
}
