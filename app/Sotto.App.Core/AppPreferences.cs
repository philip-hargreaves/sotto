using System.Text.Json;

namespace Sotto.App.Core;

/// <summary>One small json file of app preferences; absent means defaults.</summary>
public sealed class AppPreferences(string path)
{
    private sealed record Stored(bool DemoTrayEnabled, bool NpuTranscription);

    public bool DemoTrayEnabled { get; set; }

    public bool NpuTranscription { get; set; }

    public static AppPreferences Load(string path)
    {
        var preferences = new AppPreferences(path);
        try
        {
            var stored = JsonSerializer.Deserialize<Stored>(File.ReadAllText(path));
            preferences.DemoTrayEnabled = stored?.DemoTrayEnabled ?? false;
            preferences.NpuTranscription = stored?.NpuTranscription ?? false;
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
            File.WriteAllText(path, JsonSerializer.Serialize(
                new Stored(DemoTrayEnabled, NpuTranscription)));
        }
        catch (Exception)
        {
        }
    }
}
