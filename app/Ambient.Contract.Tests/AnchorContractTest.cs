using System.Text.Json;

namespace Ambient.Contract.Tests;

/// <summary>
/// The anchor/status and anchor/clear methods against the real engine.
/// </summary>
public class AnchorContractTest
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(10);

    [Fact]
    public async Task AFreshStoreHasNoVoiceprintAndClearIsIdempotent()
    {
        await using var engine = EngineProcess.Start($"LOCAL\\ambient-anchor-{Guid.NewGuid():N}");
        await using var client = await engine.ConnectAsync();

        var status = await client.RequestAsync("anchor/status", null, Timeout);
        Assert.Equal("none", status.GetProperty("origin").GetString());
        Assert.Equal(0, status.GetProperty("sessions").GetInt32());
        Assert.Equal(JsonValueKind.Null, status.GetProperty("enrolledAt").ValueKind);

        var cleared = await client.RequestAsync("anchor/clear", null, Timeout);
        Assert.Equal(JsonValueKind.Object, cleared.ValueKind);
        Assert.False(File.Exists(Path.Combine(engine.StoreRoot, "anchor.bin")));

        status = await client.RequestAsync("anchor/status", null, Timeout);
        Assert.Equal("none", status.GetProperty("origin").GetString());
    }

    [Fact]
    public async Task EnrolmentReportsProgressAndAnHonestRefusal()
    {
        // Two seconds of silence stand in for the microphone: the window elapses,
        // no speech is heard, and the outcome says so rather than seeding a print
        var wav = SessionContractTest.WriteSilenceWav();
        try
        {
            await using var engine = EngineProcess.Start(
                $"LOCAL\\ambient-enrol-{Guid.NewGuid():N}", wav);
            await using var client = await engine.ConnectAsync();
            var done = new TaskCompletionSource<JsonElement>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            var progress = 0;
            client.NotificationReceived += (method, parameters) =>
            {
                if (method == "anchor/progress")
                {
                    Interlocked.Increment(ref progress);
                }
                else if (method == "anchor/enrolled")
                {
                    done.TrySetResult(parameters.Clone());
                }
            };

            var started = await client.RequestAsync(
                "anchor/enrol", new { seconds = 1.0 }, Timeout);
            Assert.Equal(JsonValueKind.Object, started.ValueKind);

            var outcome = await done.Task.WaitAsync(Timeout);
            Assert.False(outcome.GetProperty("ok").GetBoolean());
            Assert.Contains("not enough clear speech", outcome.GetProperty("detail").GetString());
            Assert.True(progress > 0, "the level was reported while the window ran");

            var status = await client.RequestAsync("anchor/status", null, Timeout);
            Assert.Equal("none", status.GetProperty("origin").GetString());
        }
        finally
        {
            File.Delete(wav);
        }
    }
}
