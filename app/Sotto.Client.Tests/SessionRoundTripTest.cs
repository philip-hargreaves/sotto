using System.Diagnostics;

namespace Sotto.Client.Tests;

/// <summary>The full session contract against the real engine binary.</summary>
[Trait("Category", "Integration")]
public class SessionRoundTripTest
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(10);

    private static readonly string[] ExpectedNotifications = ["note/ready", "patient/ready"];

    private static string FindEngine()
    {
        for (var dir = AppContext.BaseDirectory; dir is not null; dir = Path.GetDirectoryName(dir))
        {
            foreach (var preset in new[] { "dev", "release" })
            {
                var candidate = Path.Combine(dir, "build", preset, "engine", "sotto_engine.exe");
                if (File.Exists(candidate))
                {
                    return candidate;
                }
            }
        }

        throw new FileNotFoundException(
            "sotto_engine.exe not found, build it with: cmake --workflow --preset dev");
    }

    [Fact]
    public async Task FullSessionAgainstTheRealEngine()
    {
        var pipeName = $"LOCAL\\sotto-contract-{Guid.NewGuid():N}";
        using var engine = Process.Start(new ProcessStartInfo(FindEngine(), pipeName)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
        })!;
        try
        {
            var notifications = new List<string>();
            var patientReady = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);

            var transport = await PipeTransport.ConnectAsync(pipeName, Timeout, (uint)engine.Id);
            try
            {
                transport.NotificationReceived += (method, _) =>
                {
                    lock (notifications)
                    {
                        notifications.Add(method);
                    }

                    if (method == "patient/ready")
                    {
                        patientReady.TrySetResult();
                    }
                };

                var hello = await transport.RequestAsync(
                    "engine/hello",
                    new PeerInfo("sotto-shell", "0.1.0", Protocol.ProtocolVersion),
                    Timeout);
                Assert.Equal(
                    Protocol.ProtocolVersion, hello.GetProperty("protocolVersion").GetInt32());

                await transport.RequestAsync("session/start", null, Timeout);
                await transport.RequestAsync("session/cancel", null, Timeout);
                await transport.RequestAsync("session/start", null, Timeout);
                await transport.RequestAsync("session/stop", null, Timeout);

                await patientReady.Task.WaitAsync(Timeout);
                lock (notifications)
                {
                    // Only stop produces notifications, and order is part of the contract
                    Assert.Equal(ExpectedNotifications, notifications);
                }
            }
            finally
            {
                await transport.DisposeAsync();
            }

            // Disconnect ends ServeOneClient; supervised restarts rely on this clean exit
            await engine.WaitForExitAsync(new CancellationTokenSource(Timeout).Token);
            Assert.Equal(0, engine.ExitCode);
        }
        finally
        {
            if (!engine.HasExited)
            {
                engine.Kill();
            }
        }
    }
}
