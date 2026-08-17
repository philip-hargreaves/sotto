using System.Text.Json;

namespace Sotto.Contract.Tests;

/// <summary>
/// The record, read back, delete loop against the real engine.
/// </summary>
public class SessionReadbackContractTest
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(10);

    [Fact]
    public async Task RecordedSessionsAreListedReadableAndDeletable()
    {
        var wav = SessionContractTest.WriteSilenceWav();
        try
        {
            await using var engine =
                EngineProcess.Start($"LOCAL\\sotto-readback-{Guid.NewGuid():N}", wav);
            await using var client = await engine.ConnectAsync();

            await client.RequestAsync("session/start", null, Timeout);
            await client.RequestAsync("session/stop", null, Timeout);

            var list = await client.RequestAsync("session/list", null, Timeout);
            var sessions = list.GetProperty("sessions");
            Assert.Equal(1, sessions.GetArrayLength());
            Assert.Equal("finalised", sessions[0].GetProperty("state").GetString());
            Assert.Equal(16000, sessions[0].GetProperty("sampleRate").GetInt32());
            Assert.False(string.IsNullOrEmpty(sessions[0].GetProperty("endedAt").GetString()));
            var id = sessions[0].GetProperty("id").GetString()!;

            var transcript = await client.RequestAsync(
                "session/transcript", new { id }, Timeout);
            var turns = transcript.GetProperty("turns");
            Assert.Equal(1, turns.GetArrayLength());
            Assert.StartsWith("scripted turn 0", turns[0].GetProperty("text").GetString());
            Assert.Equal(0, turns[0].GetProperty("firstFrame").GetInt64());
            // Stop may land before the fast replay finishes, so the tail holds
            // whatever had arrived; two seconds of audio is the ceiling
            Assert.InRange(turns[0].GetProperty("frameCount").GetInt64(), 1, 32000);

            await client.RequestAsync("session/delete", new { id }, Timeout);

            var after = await client.RequestAsync("session/list", null, Timeout);
            Assert.Equal(0, after.GetProperty("sessions").GetArrayLength());
            Assert.Empty(Directory.GetFiles(Path.Combine(engine.StoreRoot, "sessions")));

            await Assert.ThrowsAnyAsync<Exception>(
                () => client.RequestAsync("session/delete", new { id }, Timeout));
        }
        finally
        {
            File.Delete(wav);
        }
    }
}
