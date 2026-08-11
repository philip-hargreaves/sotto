namespace Sotto.Contract.Tests;

/// <summary>
/// The session methods and the notifications they produce, against the real
/// engine. Runs on a private pipe, so it is independent of the engine group.
/// </summary>
public class SessionContractTest
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(10);

    private static readonly string[] ExpectedNotifications = ["note/ready", "patient/ready"];

    [Fact]
    public async Task StopProducesTheNoteThenPatientNotifications()
    {
        await using var engine = EngineProcess.Start($"LOCAL\\sotto-session-{Guid.NewGuid():N}");
        var notifications = new List<string>();
        var patientReady = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);

        await using (var client = await engine.ConnectAsync())
        {
            client.NotificationReceived += (method, _) =>
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

            await client.RequestAsync("session/start", null, Timeout);
            await client.RequestAsync("session/cancel", null, Timeout);
            await client.RequestAsync("session/start", null, Timeout);
            await client.RequestAsync("session/stop", null, Timeout);

            await patientReady.Task.WaitAsync(Timeout);
            lock (notifications)
            {
                // Only stop notifies, and the order is part of the contract
                Assert.Equal(ExpectedNotifications, notifications);
            }
        }

        // Disconnect ends ServeOneClient; supervised restarts rely on this exit
        Assert.Equal(0, await engine.WaitForExitAsync(Timeout));
    }
}
