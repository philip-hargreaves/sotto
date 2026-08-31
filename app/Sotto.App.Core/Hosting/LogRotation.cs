namespace Sotto.App.Core.Hosting;

/// <summary>
/// Shifts engine.log to engine-1.log and so on, newest first, keeping the
/// last <c>keep</c> files. Called once per app run, so the log a crash was
/// writing survives the next launch instead of growing forever.
/// </summary>
public static class LogRotation
{
    public static void Rotate(string path, int keep)
    {
        try
        {
            if (!File.Exists(path))
            {
                return;
            }

            File.Delete(Shifted(path, keep - 1));
            for (var i = keep - 2; i >= 1; i--)
            {
                if (File.Exists(Shifted(path, i)))
                {
                    File.Move(Shifted(path, i), Shifted(path, i + 1), overwrite: true);
                }
            }

            File.Move(path, Shifted(path, 1), overwrite: true);
        }
        catch (Exception)
        {
            // A locked or missing log must never block the engine launch
        }
    }

    private static string Shifted(string path, int index) => Path.Combine(
        Path.GetDirectoryName(path) ?? "",
        $"{Path.GetFileNameWithoutExtension(path)}-{index}{Path.GetExtension(path)}");
}
