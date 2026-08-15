namespace Sotto.Contract.Tests;

/// <summary>
/// The session methods and the notifications they produce, against the real
/// engine. Runs on a private pipe, so it is independent of the engine group.
/// </summary>
public class SessionContractTest
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(10);

    private static readonly string[] ExpectedNotifications = ["note/ready", "patient/ready"];

    // Two seconds of PCM16 silence: sessions replay it instead of a microphone
    private static string WriteSilenceWav()
    {
        const int frames = 2 * 16000;
        var bytes = new byte[44 + frames * 2];
        void Tag(int offset, string tag) =>
            System.Text.Encoding.ASCII.GetBytes(tag).CopyTo(bytes, offset);
        void U32(int offset, uint value) =>
            BitConverter.GetBytes(value).CopyTo(bytes, offset);
        void U16(int offset, ushort value) =>
            BitConverter.GetBytes(value).CopyTo(bytes, offset);
        Tag(0, "RIFF");
        U32(4, (uint)(bytes.Length - 8));
        Tag(8, "WAVE");
        Tag(12, "fmt ");
        U32(16, 16);
        U16(20, 1);
        U16(22, 1);
        U32(24, 16000);
        U32(28, 32000);
        U16(32, 2);
        U16(34, 16);
        Tag(36, "data");
        U32(40, (uint)(frames * 2));

        var path = Path.Combine(
            Path.GetTempPath(), $"sotto-silence-{Guid.NewGuid():N}.wav");
        File.WriteAllBytes(path, bytes);
        return path;
    }

    [Fact]
    public async Task StopProducesTheNoteThenPatientNotifications()
    {
        var wav = WriteSilenceWav();
        try
        {
            await using var engine =
                EngineProcess.Start($"LOCAL\\sotto-session-{Guid.NewGuid():N}", wav);
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
                    // Sessions now stream levels; the pipeline ordering holds among the rest
                    Assert.Contains("audio.level", notifications);
                    Assert.Equal(
                        ExpectedNotifications,
                        notifications.Where(n => n != "audio.level").ToArray());
                }
            }

            // Disconnect ends ServeOneClient; supervised restarts rely on this exit
            Assert.Equal(0, await engine.WaitForExitAsync(Timeout));

            // Two sessions ran: the cancelled one left nothing, the stopped one
            // is on disk encrypted with its key beside it
            var sessionsDir = Path.Combine(engine.StoreRoot, "sessions");
            Assert.True(File.Exists(Path.Combine(engine.StoreRoot, "main.db")));
            Assert.Single(Directory.GetFiles(sessionsDir, "*.db"));
            Assert.Single(Directory.GetFiles(sessionsDir, "*.key"));
        }
        finally
        {
            File.Delete(wav);
        }
    }
}
