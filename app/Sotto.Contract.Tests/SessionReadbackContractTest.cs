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
            // The note and sheet follow the seal on their own thread; wait for them
            var patientReady = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            client.NotificationReceived += (method, _) =>
            {
                if (method == "patient/ready")
                {
                    patientReady.TrySetResult();
                }
            };

            await client.RequestAsync("session/start", null, Timeout);
            await client.RequestAsync("session/stop", null, Timeout);
            await patientReady.Task.WaitAsync(Timeout);

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

            // The CI engine has no note model: the record starts empty, with
            // every field present, and fills through the clinician's edits
            var note = await client.RequestAsync("session/note", new { id }, Timeout);
            Assert.Equal("", note.GetProperty("text").GetString());
            Assert.Equal(JsonValueKind.Null, note.GetProperty("generatedAt").ValueKind);
            Assert.Equal(JsonValueKind.Null, note.GetProperty("editedAt").ValueKind);
            Assert.Equal("", sessions[0].GetProperty("label").GetString());

            await client.RequestAsync("note/update", new { id, text = "edited" }, Timeout);
            note = await client.RequestAsync("session/note", new { id }, Timeout);
            Assert.Equal("edited", note.GetProperty("text").GetString());
            Assert.False(string.IsNullOrEmpty(note.GetProperty("editedAt").GetString()));

            var patient = await client.RequestAsync("session/patient", new { id }, Timeout);
            Assert.Equal("en", patient.GetProperty("language").GetString());
            Assert.Equal(JsonValueKind.Null, patient.GetProperty("translation").ValueKind);

            await client.RequestAsync("session/label", new { id, text = "Elbow swelling" }, Timeout);
            var relisted = await client.RequestAsync("session/list", null, Timeout);
            var row = relisted.GetProperty("sessions")[0];
            Assert.Equal("Elbow swelling", row.GetProperty("label").GetString());
            Assert.False(string.IsNullOrEmpty(row.GetProperty("editedAt").GetString()));

            // Review: a past session reopens for regeneration; the CI engine
            // has no note model, so regenerate is refused cleanly rather than
            // crashing, and close ends the review
            await client.RequestAsync("session/open", new { id }, Timeout);
            await Assert.ThrowsAnyAsync<Exception>(
                () => client.RequestAsync(
                    "note/regenerate", new { style = "prose", detail = "standard" }, Timeout));
            await client.RequestAsync("session/close", null, Timeout);
            await Assert.ThrowsAnyAsync<Exception>(
                () => client.RequestAsync("session/open", new { id = "nope" }, Timeout));

            await client.RequestAsync("session/delete", new { id }, Timeout);

            var after = await client.RequestAsync("session/list", null, Timeout);
            Assert.Equal(0, after.GetProperty("sessions").GetArrayLength());

            // Keep consultations off: readable by id until the consultation
            // is left, but never in history - and erased at close
            await client.RequestAsync("session/start", new { retain = false }, Timeout);
            var unretainedId = (await client.RequestAsync("session/stop", null, Timeout))
                .GetProperty("sessionId").GetString()!;
            var unretained = await client.RequestAsync("session/list", null, Timeout);
            Assert.Equal(0, unretained.GetProperty("sessions").GetArrayLength());
            var transcript2 = await client.RequestAsync(
                "session/transcript", new { id = unretainedId }, Timeout);
            Assert.True(transcript2.GetProperty("turns").GetArrayLength() > 0);
            await client.RequestAsync("session/close", null, Timeout);
            await Assert.ThrowsAnyAsync<Exception>(
                () => client.RequestAsync("session/transcript", new { id = unretainedId }, Timeout));
            Assert.True(File.Exists(Path.Combine(engine.StoreRoot, "sotto.db")));
            Assert.False(Directory.Exists(Path.Combine(engine.StoreRoot, "sessions")));

            await Assert.ThrowsAnyAsync<Exception>(
                () => client.RequestAsync("session/delete", new { id }, Timeout));
        }
        finally
        {
            File.Delete(wav);
        }
    }
}
