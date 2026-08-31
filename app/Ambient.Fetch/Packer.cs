using System.Text.Json;

namespace Ambient.Fetch;

/// <summary>
/// Prepares one pack for publishing: shards files over the asset cap into
/// an upload directory and writes the registry entry that Fetcher consumes.
/// </summary>
public static class Packer
{
    // Release assets cap at 2 GiB; leave headroom
    public const long ShardBytes = (long)(1.9 * 1024 * 1024 * 1024);

    public static Pack Pack(string sourceDir, string id, string baseUrl, string target,
        string uploadDir, long shardBytes = ShardBytes)
    {
        Directory.CreateDirectory(uploadDir);
        var files = new Dictionary<string, string>();
        var shards = new Dictionary<string, List<Shard>>();
        JsonElement? manifest = null;

        foreach (var path in Directory.EnumerateFiles(sourceDir, "*", SearchOption.AllDirectories))
        {
            var relative = Path.GetRelativePath(sourceDir, path).Replace('\\', '/');
            if (relative.StartsWith(".cache", StringComparison.Ordinal))
            {
                continue;
            }

            if (relative == "manifest.json")
            {
                manifest = JsonDocument.Parse(File.ReadAllText(path)).RootElement.Clone();
                continue;
            }

            files[relative] = Hashing.Sha256File(path);
            shards[relative] = Split(path, $"{id}--{relative.Replace('/', '_')}", uploadDir,
                shardBytes);
        }

        var pack = new Pack(id, baseUrl, target, files, shards, manifest);
        pack.Save(Path.Combine(uploadDir, $"{id}.json"));
        return pack;
    }

    private static List<Shard> Split(string path, string assetName, string uploadDir,
        long shardBytes)
    {
        var length = new FileInfo(path).Length;
        if (length <= shardBytes)
        {
            var single = Path.Combine(uploadDir, assetName);
            File.Copy(path, single, overwrite: true);
            return [new Shard(assetName, length, Hashing.Sha256File(single))];
        }

        var shards = new List<Shard>();
        using var source = File.OpenRead(path);
        var buffer = new byte[1 << 20];
        for (var part = 1; source.Position < length; part++)
        {
            var name = $"{assetName}.part{part:00}";
            var shardPath = Path.Combine(uploadDir, name);
            long written = 0;
            using (var shard = File.Create(shardPath))
            {
                while (written < shardBytes && source.Position < length)
                {
                    var take = (int)Math.Min(buffer.Length, shardBytes - written);
                    var read = source.Read(buffer, 0, take);
                    shard.Write(buffer, 0, read);
                    written += read;
                }
            }

            shards.Add(new Shard(name, written, Hashing.Sha256File(shardPath)));
        }

        return shards;
    }
}
