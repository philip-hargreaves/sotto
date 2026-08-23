using System.Text.Json;

namespace Sotto.App.Core.ViewModels;

/// <summary>One partner mark; Dark falls back to the light artwork.</summary>
public sealed record CreditMark(string Name, string LightPath, string DarkPath, double Height);

/// <summary>
/// The credits row, driven by Assets/logos/credits.json so a showcase can add
/// or remove marks by editing the deployed folder, never the code. A missing
/// file skips its mark; a missing or broken manifest yields an empty row.
/// </summary>
public sealed class CreditsViewModel
{
    public IReadOnlyList<CreditMark> Marks { get; }

    public CreditsViewModel(string? logosDirectory = null)
    {
        var directory = logosDirectory
            ?? Path.Combine(AppContext.BaseDirectory, "Assets", "logos");
        Marks = Load(directory);
    }

    private static List<CreditMark> Load(string directory)
    {
        var manifest = Path.Combine(directory, "credits.json");
        if (!File.Exists(manifest))
        {
            return [];
        }

        try
        {
            using var parsed = JsonDocument.Parse(File.ReadAllText(manifest));
            var marks = new List<CreditMark>();
            foreach (var entry in parsed.RootElement.GetProperty("marks").EnumerateArray())
            {
                var light = entry.GetProperty("light").GetString() ?? "";
                var lightPath = Path.Combine(directory, light);
                if (!File.Exists(lightPath))
                {
                    Console.Error.WriteLine($"credits: {light} missing, mark skipped");
                    continue;
                }

                var dark = entry.TryGetProperty("dark", out var d) ? d.GetString() : null;
                var darkPath = dark is null ? lightPath : Path.Combine(directory, dark);
                marks.Add(new CreditMark(
                    entry.GetProperty("name").GetString() ?? "",
                    lightPath,
                    File.Exists(darkPath) ? darkPath : lightPath,
                    entry.TryGetProperty("height", out var h) ? h.GetDouble() : 16));
            }

            return marks;
        }
        catch (JsonException e)
        {
            Console.Error.WriteLine($"credits: manifest unreadable ({e.Message})");
            return [];
        }
    }
}
