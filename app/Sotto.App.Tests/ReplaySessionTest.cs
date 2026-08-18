using System.Text.Json;
using Sotto.App.Core.Hosting;
using Sotto.Client;

namespace Sotto.App.Tests;

/// <summary>
/// "Press play" through the whole shell stack against the real engine with
/// real models: connect, idle, replay, pause, stop.
/// </summary>
[Trait("Category", "Integration")]
[Trait("Requires", "Engine")]
public class ReplaySessionTest
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(15);

    private sealed class FakeSession : ISessionState
    {
        public bool ConsultationActive { get; set; }
    }

    [Fact]
    public async Task PlayAfterIdleReplaysAndFinalises()
    {
        var pipeName = $"LOCAL\\sotto-replay-{Guid.NewGuid():N}";
        var directory = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        Directory.CreateDirectory(directory);
        var wav = SessionContractWav.Write(seconds: 5);
        // Real models when present: startup cost and code paths must match
        // the shipped app, not a scripted stand-in
        var models = FindModels();
        using var launcher = new ProcessEngineLauncher(
            FindEngine(),
            $"{pipeName} \"{Path.Combine(directory, "store")}\""
                + (models is null ? "" : $" \"{models}\""));
        using var host = new EngineSupervisor(
            launcher, new FakeSession(), TimeProvider.System,
            new FileCrashLog(Path.Combine(directory, "crashes.jsonl")));
        await using var connection = new EngineConnection(host, async (pid, ct) =>
            await PipeTransport.ConnectAsync(pipeName, Timeout, pid, ct));
        try
        {
            host.Start();
            try
            {
                await RetryAsync(() => connection.RequestAsync("engine/echo", new { payload = "up" }, Timeout));
            }
            catch (IOException e)
            {
                var crashes = File.Exists(Path.Combine(directory, "crashes.jsonl"))
                    ? File.ReadAllText(Path.Combine(directory, "crashes.jsonl"))
                    : "<none>";
                throw new IOException(
                    $"echo never connected. status={host.Status} fault={host.Fault?.Kind} "
                    + $"pid={host.EnginePid} crashes={crashes}", e);
            }
            var pidAtStart = host.EnginePid;

            // The idle window between launch and play is where the silent
            // connection death lived
            await Task.Delay(TimeSpan.FromSeconds(20));
            Assert.Equal(pidAtStart, host.EnginePid);

            var noteReady = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            connection.NotificationReceived += (method, _) =>
            {
                if (method == "note/ready")
                {
                    noteReady.TrySetResult();
                }
            };

            await connection.RequestAsync(
                "session/start",
                new { replay = new { path = wav, speed = 1.0, monitor = false } }, Timeout);
            await Task.Delay(TimeSpan.FromSeconds(2));
            await connection.RequestAsync("session/pause", new { paused = true }, Timeout);
            await connection.RequestAsync("session/pause", new { paused = false }, Timeout);
            await connection.RequestAsync("session/stop", null, TimeSpan.FromSeconds(60));
            await noteReady.Task.WaitAsync(Timeout);
        }
        finally
        {
            host.Shutdown();
            try
            {
                File.Delete(wav);
                Directory.Delete(directory, recursive: true);
            }
            catch (IOException)
            {
                // The engine may still hold the store for a beat; temp cleans itself
            }
        }
    }

    private static string? FindModels()
    {
        for (var dir = AppContext.BaseDirectory; dir is not null; dir = Path.GetDirectoryName(dir))
        {
            var models = Path.Combine(dir, "models");
            if (Directory.Exists(models))
            {
                return models;
            }
        }

        return null;
    }

    private static string FindEngine()
    {
        for (var dir = AppContext.BaseDirectory; dir is not null; dir = Path.GetDirectoryName(dir))
        {
            var buildRoot = Path.Combine(dir, "build");
            if (Directory.Exists(buildRoot))
            {
                var newest = Directory
                    .EnumerateFiles(buildRoot, "sotto_engine.exe", SearchOption.AllDirectories)
                    .Select(path => new FileInfo(path))
                    .OrderByDescending(info => info.LastWriteTimeUtc)
                    .FirstOrDefault();
                if (newest is not null)
                {
                    return newest.FullName;
                }
            }
        }

        throw new FileNotFoundException("sotto_engine.exe not found");
    }

    private static async Task<JsonElement> RetryAsync(Func<Task<JsonElement>> request)
    {
        for (var attempt = 0; ; attempt++)
        {
            try
            {
                return await request();
            }
            catch (IOException e) when (attempt < 600)  // model init takes seconds
            {
                if (attempt == 599)
                {
                    throw new IOException($"gave up: {e}", e);
                }

                await Task.Delay(50);
            }
        }
    }
}
