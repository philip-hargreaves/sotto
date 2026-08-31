namespace Sotto.App.Tests;

/// <summary>
/// Locates the engine binary for real-engine tests: an explicit override,
/// else the release build, else the newest binary under any preset. Release
/// is preferred to match the shipped app — and because OpenVINO's debug GPU
/// plugin asserts on the second generation of the note/patient lane.
/// </summary>
public static class EnginePath
{
    public static string Find()
    {
        var overridePath = Environment.GetEnvironmentVariable("SOTTO_ENGINE_PATH");
        if (!string.IsNullOrEmpty(overridePath))
        {
            return overridePath;
        }

        const string exe = "sotto_engine.exe";
        for (var dir = AppContext.BaseDirectory; dir is not null; dir = Path.GetDirectoryName(dir))
        {
            var buildRoot = Path.Combine(dir, "build");
            if (!Directory.Exists(buildRoot))
            {
                continue;
            }

            var release = Path.Combine(buildRoot, "release", "engine", exe);
            if (File.Exists(release))
            {
                return release;
            }

            var newest = Directory
                .EnumerateFiles(buildRoot, exe, SearchOption.AllDirectories)
                .Select(path => new FileInfo(path))
                .OrderByDescending(info => info.LastWriteTimeUtc)
                .FirstOrDefault();
            if (newest is not null)
            {
                return newest.FullName;
            }
        }

        throw new FileNotFoundException(
            $"{exe} not found, build it with: cmake --workflow --preset release");
    }
}
