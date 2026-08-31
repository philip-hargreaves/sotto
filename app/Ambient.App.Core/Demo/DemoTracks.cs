using System.Text.Json;

namespace Ambient.App.Core.Demo;

/// <summary>A bundled replay recording: name, wav path, "name (m:ss)" label.</summary>
public sealed record DemoTrack(string Name, string Path)
{
    public string Display { get; } = Label(Name, Path);

    private static string Label(string name, string path)
    {
        var seconds = (int)Math.Round(DemoTracks.DurationSeconds(path));
        return seconds <= 0 ? name : $"{name} ({seconds / 60}:{seconds % 60:00})";
    }
}

public static class DemoTracks
{
    /// <summary>
    /// Loads demo/tracks.json, probing upward so packaged and dev layouts
    /// both resolve. Missing manifest or wavs simply shrink the list.
    /// </summary>
    public static IReadOnlyList<DemoTrack> Load(string? baseDirectory = null)
    {
        // Packaged debug runs sit one level deeper (AppX), so probe generously
        var dir = baseDirectory ?? AppContext.BaseDirectory;
        for (var i = 0; i < 10 && dir is not null; i++, dir = Directory.GetParent(dir)?.FullName)
        {
            var manifest = System.IO.Path.Combine(dir, "demo", "tracks.json");
            if (File.Exists(manifest))
            {
                return Parse(manifest);
            }
        }

        return [];
    }

    public static IReadOnlyList<DemoTrack> Parse(string manifestPath)
    {
        try
        {
            var root = System.IO.Path.GetDirectoryName(manifestPath)!;
            using var json = JsonDocument.Parse(File.ReadAllText(manifestPath));
            var tracks = new List<DemoTrack>();
            foreach (var entry in json.RootElement.GetProperty("tracks").EnumerateArray())
            {
                var name = entry.GetProperty("name").GetString();
                var file = entry.GetProperty("file").GetString();
                if (string.IsNullOrEmpty(name) || string.IsNullOrEmpty(file))
                {
                    continue;
                }

                var path = System.IO.Path.Combine(root, file);
                if (File.Exists(path))
                {
                    tracks.Add(new DemoTrack(name, path));
                }
            }

            return tracks;
        }
        catch (Exception)
        {
            return [];
        }
    }

    /// <summary>
    /// Duration of a 16 kHz mono wav, from its header alone. 0 when unreadable.
    /// </summary>
    public static double DurationSeconds(string path)
    {
        try
        {
            using var stream = File.OpenRead(path);
            using var reader = new BinaryReader(stream);
            if (new string(reader.ReadChars(4)) != "RIFF")
            {
                return 0;
            }

            reader.ReadUInt32();
            if (new string(reader.ReadChars(4)) != "WAVE")
            {
                return 0;
            }

            ushort bits = 16;
            while (stream.Position + 8 <= stream.Length)
            {
                var tag = new string(reader.ReadChars(4));
                var size = reader.ReadUInt32();
                if (tag == "fmt ")
                {
                    var chunk = reader.ReadBytes((int)size + (int)(size & 1));
                    bits = BitConverter.ToUInt16(chunk, 14);
                }
                else if (tag == "data")
                {
                    return size / (16000.0 * (bits / 8.0));
                }
                else
                {
                    stream.Seek(size + (size & 1), SeekOrigin.Current);
                }
            }

            return 0;
        }
        catch (Exception)
        {
            return 0;
        }
    }
}
