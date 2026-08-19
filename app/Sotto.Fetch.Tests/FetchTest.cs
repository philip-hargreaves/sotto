using System.Net;
using Sotto.Fetch;

namespace Sotto.Fetch.Tests;

/// <summary>Serves a directory over loopback HTTP with range support.</summary>
public sealed class AssetServer : IDisposable
{
    private readonly HttpListener _listener = new();
    private readonly string _dir;
    private readonly CancellationTokenSource _stop = new();

    private int _requests;

    public int Requests => _requests;

    public AssetServer(string dir)
    {
        _dir = dir;
        var port = Random.Shared.Next(20000, 60000);
        BaseUrl = $"http://127.0.0.1:{port}";
        _listener.Prefixes.Add(BaseUrl + "/");
        _listener.Start();
        _ = Task.Run(ServeAsync);
    }

    public string BaseUrl { get; }

    private async Task ServeAsync()
    {
        while (!_stop.IsCancellationRequested)
        {
            HttpListenerContext context;
            try
            {
                context = await _listener.GetContextAsync();
            }
            catch (Exception)
            {
                return;
            }

            Interlocked.Increment(ref _requests);
            var path = Path.Combine(_dir, context.Request.Url!.AbsolutePath.TrimStart('/'));
            if (!File.Exists(path))
            {
                context.Response.StatusCode = 404;
                context.Response.Close();
                continue;
            }

            var bytes = File.ReadAllBytes(path);
            var from = 0L;
            var range = context.Request.Headers["Range"];
            if (range is not null && range.StartsWith("bytes=", StringComparison.Ordinal))
            {
                from = long.Parse(range["bytes=".Length..].TrimEnd('-'),
                    System.Globalization.CultureInfo.InvariantCulture);
                context.Response.StatusCode = 206;
            }

            context.Response.ContentLength64 = bytes.Length - from;
            await context.Response.OutputStream.WriteAsync(bytes.AsMemory((int)from));
            context.Response.Close();
        }
    }

    public void Dispose()
    {
        _stop.Cancel();
        _listener.Stop();
        GC.SuppressFinalize(this);
    }
}

public class FetchTest : IDisposable
{
    private readonly string _root = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
    private readonly string _source;
    private readonly string _upload;
    private readonly string _store;

    public FetchTest()
    {
        _source = Path.Combine(_root, "source");
        _upload = Path.Combine(_root, "upload");
        _store = Path.Combine(_root, "store");
        Directory.CreateDirectory(_source);
        Directory.CreateDirectory(_store);

        var random = new Random(42);
        var big = new byte[600_000];
        random.NextBytes(big);
        File.WriteAllBytes(Path.Combine(_source, "weights.bin"), big);
        File.WriteAllText(Path.Combine(_source, "config.json"), "{\"layers\": 3}");
        File.WriteAllText(Path.Combine(_source, "manifest.json"), "{\"id\": \"tiny\"}");
    }

    public void Dispose()
    {
        try
        {
            Directory.Delete(_root, recursive: true);
        }
        catch (IOException)
        {
        }

        GC.SuppressFinalize(this);
    }

    private Pack PackTiny(string baseUrl) =>
        Packer.Pack(_source, "tiny", baseUrl, "models/tiny", _upload, shardBytes: 250_000);

    private static Fetcher NewFetcher() => new(new HttpClient(), _ => { });

    private void AssertInstalled()
    {
        var target = Path.Combine(_store, "models", "tiny");
        Assert.Equal(Hashing.Sha256File(Path.Combine(_source, "weights.bin")),
            Hashing.Sha256File(Path.Combine(target, "weights.bin")));
        Assert.Equal(Hashing.Sha256File(Path.Combine(_source, "config.json")),
            Hashing.Sha256File(Path.Combine(target, "config.json")));
        Assert.Contains("tiny", File.ReadAllText(Path.Combine(target, "manifest.json")));
        Assert.False(Directory.Exists(Path.Combine(_store, ".fetch")), "work dir is cleaned");
    }

    [Fact]
    public void PackShardsAndRoundTripsTheRegistry()
    {
        var pack = PackTiny("http://example");

        Assert.Equal(3, pack.Shards["weights.bin"].Count);
        Assert.Single(pack.Shards["config.json"]);
        Assert.NotNull(pack.Manifest);

        var reloaded = Pack.Load(Path.Combine(_upload, "tiny.json"));
        Assert.Equal(pack.Files, reloaded.Files);
        Assert.Equal(pack.Shards["weights.bin"], reloaded.Shards["weights.bin"]);
        Assert.Equal(pack.Target, reloaded.Target);
    }

    [Fact]
    public async Task FetchInstallsVerifiedAndIsIdempotent()
    {
        using var server = new AssetServer(_upload);
        var pack = PackTiny(server.BaseUrl);

        Assert.True(await NewFetcher().InstallAsync(pack, _store));
        AssertInstalled();

        var before = server.Requests;
        Assert.False(await NewFetcher().InstallAsync(pack, _store), "a valid store is skipped");
        Assert.Equal(before, server.Requests);
    }

    [Fact]
    public async Task ATruncatedShardResumesAndVerifies()
    {
        using var server = new AssetServer(_upload);
        var pack = PackTiny(server.BaseUrl);

        // A previous run died mid-shard
        var work = Path.Combine(_store, ".fetch", "tiny");
        Directory.CreateDirectory(work);
        var shard = pack.Shards["weights.bin"][0];
        var full = File.ReadAllBytes(Path.Combine(_upload, shard.Name));
        File.WriteAllBytes(Path.Combine(work, shard.Name), full[..100_000]);

        Assert.True(await NewFetcher().InstallAsync(pack, _store));
        AssertInstalled();
    }

    [Fact]
    public async Task AStagedFileResumesFromTheShardBoundary()
    {
        using var server = new AssetServer(_upload);
        var pack = PackTiny(server.BaseUrl);

        // A previous run staged the first shard whole, then died
        var stage = Path.Combine(_store, ".fetch", "tiny", "stage");
        Directory.CreateDirectory(stage);
        var first = pack.Shards["weights.bin"][0];
        File.WriteAllBytes(Path.Combine(stage, "weights.bin"),
            File.ReadAllBytes(Path.Combine(_upload, first.Name)));

        Assert.True(await NewFetcher().InstallAsync(pack, _store));
        AssertInstalled();
        Assert.Equal(3, server.Requests);  // two remaining shards + config, never the first
    }

    [Fact]
    public async Task ACorruptAssetFailsAndInstallsNothing()
    {
        using var server = new AssetServer(_upload);
        var pack = PackTiny(server.BaseUrl);
        var shard = pack.Shards["weights.bin"][1];
        var bytes = File.ReadAllBytes(Path.Combine(_upload, shard.Name));
        bytes[123] ^= 0xFF;
        File.WriteAllBytes(Path.Combine(_upload, shard.Name), bytes);

        await Assert.ThrowsAsync<InvalidDataException>(
            () => NewFetcher().InstallAsync(pack, _store));
        Assert.False(Directory.Exists(Path.Combine(_store, "models", "tiny")));
    }

    [Fact]
    public async Task ACorruptedInstallIsRepairedOnTheNextRun()
    {
        using var server = new AssetServer(_upload);
        var pack = PackTiny(server.BaseUrl);
        Assert.True(await NewFetcher().InstallAsync(pack, _store));

        var installed = Path.Combine(_store, "models", "tiny", "weights.bin");
        File.WriteAllText(installed, "damaged");

        Assert.True(await NewFetcher().InstallAsync(pack, _store));
        AssertInstalled();
    }
}
