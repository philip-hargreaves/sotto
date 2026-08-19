using System.Net;
using System.Text.Json;

namespace Sotto.Fetch;

/// <summary>
/// Downloads and installs one pack: shards resume from the bytes on disk,
/// every shard and every reassembled file is hash-verified, and the pack
/// only reaches its target directory whole - a failure leaves the previous
/// state untouched and the next run continues where this one stopped.
/// </summary>
public sealed class Fetcher(HttpClient http, Action<string> log)
{
    private const int Attempts = 4;

    /// <summary>False when the target already verifies and nothing was fetched.</summary>
    public async Task<bool> InstallAsync(Pack pack, string root, CancellationToken ct = default)
    {
        var target = Path.Combine(root, pack.Target);
        if (Verifies(pack, target))
        {
            log($"{pack.Id}: already installed");
            return false;
        }

        var work = Path.Combine(root, ".fetch", pack.Id);
        var stage = Path.Combine(work, "stage");
        Directory.CreateDirectory(work);
        if (Directory.Exists(stage))
        {
            Directory.Delete(stage, recursive: true);
        }

        Directory.CreateDirectory(stage);

        foreach (var (file, shards) in pack.Shards)
        {
            foreach (var shard in shards)
            {
                await DownloadShardAsync(pack, shard, work, ct).ConfigureAwait(false);
            }

            Reassemble(pack, file, shards, work, stage);
        }

        if (pack.Manifest is not null)
        {
            File.WriteAllText(Path.Combine(stage, "manifest.json"),
                JsonSerializer.Serialize(pack.Manifest, Json.Options));
        }

        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        if (Directory.Exists(target))
        {
            Directory.Delete(target, recursive: true);
        }

        Directory.Move(stage, target);
        Directory.Delete(work, recursive: true);
        var fetchRoot = Path.GetDirectoryName(work)!;
        if (!Directory.EnumerateFileSystemEntries(fetchRoot).Any())
        {
            Directory.Delete(fetchRoot);
        }
        log($"{pack.Id}: installed and verified");
        return true;
    }

    private static bool Verifies(Pack pack, string target)
    {
        if (!Directory.Exists(target))
        {
            return false;
        }

        if (pack.Manifest is not null && !File.Exists(Path.Combine(target, "manifest.json")))
        {
            return false;
        }

        foreach (var (file, sha256) in pack.Files)
        {
            var path = Path.Combine(target, file);
            if (!File.Exists(path) || Hashing.Sha256File(path) != sha256)
            {
                return false;
            }
        }

        return true;
    }

    private async Task DownloadShardAsync(Pack pack, Shard shard, string work,
        CancellationToken ct)
    {
        var path = Path.Combine(work, shard.Name);
        for (var attempt = 1; ; attempt++)
        {
            try
            {
                var have = File.Exists(path) ? new FileInfo(path).Length : 0;
                if (have > shard.Size)
                {
                    File.Delete(path);
                    have = 0;
                }

                if (have < shard.Size)
                {
                    await DownloadRangeAsync(
                        $"{pack.BaseUrl}/{shard.Name}", path, have, shard.Size, pack.Id, ct)
                        .ConfigureAwait(false);
                }

                if (Hashing.Sha256File(path) == shard.Sha256)
                {
                    return;
                }

                // A corrupt shard cannot be resumed into a good one
                File.Delete(path);
                throw new InvalidDataException($"{shard.Name}: hash mismatch after download");
            }
            catch (Exception e) when (attempt < Attempts && e is not OperationCanceledException)
            {
                log($"{pack.Id}: {shard.Name} attempt {attempt} failed ({e.Message}); retrying");
                await Task.Delay(TimeSpan.FromSeconds(attempt), ct).ConfigureAwait(false);
            }
        }
    }

    private async Task DownloadRangeAsync(string url, string path, long from, long total,
        string id, CancellationToken ct)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, url);
        if (from > 0)
        {
            request.Headers.Range = new System.Net.Http.Headers.RangeHeaderValue(from, null);
        }

        using var response =
            await http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, ct)
                .ConfigureAwait(false);
        response.EnsureSuccessStatusCode();
        if (from > 0 && response.StatusCode != HttpStatusCode.PartialContent)
        {
            from = 0;  // the server ignored the range; start over
        }

        await using var file = new FileStream(
            path, from > 0 ? FileMode.Open : FileMode.Create, FileAccess.Write);
        file.Seek(from, SeekOrigin.Begin);
        await using var body = await response.Content.ReadAsStreamAsync(ct).ConfigureAwait(false);
        var buffer = new byte[1 << 20];
        long done = from;
        var logged = -1L;
        int read;
        while ((read = await body.ReadAsync(buffer, ct).ConfigureAwait(false)) > 0)
        {
            await file.WriteAsync(buffer.AsMemory(0, read), ct).ConfigureAwait(false);
            done += read;
            var percent = 100 * done / Math.Max(total, 1);
            if (percent / 5 > logged)
            {
                logged = percent / 5;
                log($"{id}: {Path.GetFileName(path)} {percent}%");
            }
        }
    }

    private static void Reassemble(Pack pack, string file, List<Shard> shards, string work,
        string stage)
    {
        var path = Path.Combine(stage, file);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        using (var output = File.Create(path))
        {
            foreach (var shard in shards)
            {
                using var input = File.OpenRead(Path.Combine(work, shard.Name));
                input.CopyTo(output);
            }
        }

        var actual = Hashing.Sha256File(path);
        if (actual != pack.Files[file])
        {
            throw new InvalidDataException($"{file}: reassembled hash mismatch");
        }
    }
}
