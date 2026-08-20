using System.Diagnostics;
using System.Text.Json;
using Sotto.App.Core.Hosting;
using Sotto.Client;

namespace Sotto.App.Tests;

/// <summary>
/// The whole shell stack against the real engine: supervisor, launcher and
/// connection together. A kill mid-idle must heal without a new client.
/// </summary>
[Collection("engine")]
[Trait("Category", "Integration")]
[Trait("Requires", "Engine")]
public class TransportRecoveryTest
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(10);

    private sealed class FakeSession : ISessionState
    {
        public bool ConsultationActive { get; set; }
    }

    private static string FindEngine() => EnginePath.Find();

    // Requests fail fast while the connection is (re)dialling, so poll
    private static async Task<JsonElement> RetryAsync(Func<Task<JsonElement>> request)
    {
        for (var attempt = 0; ; attempt++)
        {
            try
            {
                return await request();
            }
            catch (IOException) when (attempt < 100)
            {
                await Task.Delay(50);
            }
        }
    }

    [Fact]
    public async Task TheConnectionSurvivesASupervisedRestart()
    {
        var pipeName = $"LOCAL\\sotto-e2e-{Guid.NewGuid():N}";
        var directory = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        var crashPath = Path.Combine(directory, "crashes.jsonl");
        using var launcher = new ProcessEngineLauncher(FindEngine(), pipeName);
        using var host = new EngineSupervisor(
            launcher, new FakeSession(), TimeProvider.System, new FileCrashLog(crashPath));
        await using var connection = new EngineConnection(host, async (pid, ct) =>
            await PipeTransport.ConnectAsync(pipeName, Timeout, pid, ct));
        try
        {
            // Echo, not a session: this test is about the transport, and echo
            // needs no microphone on a CI runner
            host.Start();
            await RetryAsync(() => connection.RequestAsync("engine/echo", new { payload = "up" }, Timeout));
            var firstPid = host.EnginePid;
            Assert.NotNull(firstPid);

            Process.GetProcessById(firstPid.Value).Kill();

            await RetryAsync(() => connection.RequestAsync("engine/echo", new { payload = "back" }, Timeout));
            Assert.NotEqual(firstPid, host.EnginePid);
            Assert.Single(File.ReadAllLines(crashPath));

            var patientReady = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            connection.NotificationReceived += (method, _) =>
            {
                if (method == "patient/ready")
                {
                    patientReady.TrySetResult();
                }
            };
            await connection.RequestAsync("session/stop", null, Timeout);
            await patientReady.Task.WaitAsync(Timeout);
        }
        finally
        {
            host.Shutdown();
            if (Directory.Exists(directory))
            {
                Directory.Delete(directory, recursive: true);
            }
        }
    }
}
