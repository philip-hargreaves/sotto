using System.Text.Json;
using System.Text.Json.Serialization;

namespace Sotto.App.Core.Hosting;

/// <summary>
/// Appends one JSON line per crash. The file holds only what CrashReport
/// carries, so it is safe to include in a support bundle.
/// </summary>
public sealed class FileCrashLog(string path) : ICrashLog
{
    private static readonly JsonSerializerOptions Options = new()
    {
        Converters = { new JsonStringEnumConverter() },
    };

    public void Record(CrashReport report)
    {
        try
        {
            var directory = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(directory))
            {
                Directory.CreateDirectory(directory);
            }

            File.AppendAllText(
                path, JsonSerializer.Serialize(report, Options) + Environment.NewLine);
        }
        catch (Exception)
        {
            // Logging must never take the app down
        }
    }
}
