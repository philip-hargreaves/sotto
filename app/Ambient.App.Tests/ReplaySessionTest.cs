using System.Diagnostics;
using System.Text.Json;
using Ambient.App.Core.Hosting;
using Ambient.App.Core.ViewModels;
using Ambient.Client;

namespace Ambient.App.Tests;

/// <summary>
/// "Press play" through the whole shell stack against the real engine with
/// real models: connect, idle, replay, pause, stop.
/// </summary>
[Collection("engine")]
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
        var pipeName = $"LOCAL\\ambient-replay-{Guid.NewGuid():N}";
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

            // A silent wav has no transcript, so the real note lane reports
            // failure; either signal proves the pipeline answered
            var noteDone = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            connection.NotificationReceived += (method, _) =>
            {
                if (method is "note/ready" or "note/failed")
                {
                    noteDone.TrySetResult();
                }
            };

            await connection.RequestAsync(
                "session/start",
                new { replay = new { path = wav, speed = 1.0, monitor = false } }, Timeout);
            await Task.Delay(TimeSpan.FromSeconds(2));
            await connection.RequestAsync("session/pause", new { paused = true }, Timeout);
            await connection.RequestAsync("session/pause", new { paused = false }, Timeout);
            await connection.RequestAsync("session/stop", null, TimeSpan.FromSeconds(60));
            await noteDone.Task.WaitAsync(TimeSpan.FromSeconds(120));
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

    [Fact]
    public async Task ARealTrackFinalisesToALabelledTranscript()
    {
        var track = Path.Combine(
            Path.GetDirectoryName(FindModels()) ?? "", "demo", "day2_consultation02_mixed.wav");
        if (!File.Exists(track))
        {
            return;  // demo tracks not staged on this machine
        }

        var pipeName = $"LOCAL\\ambient-track-{Guid.NewGuid():N}";
        var directory = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        Directory.CreateDirectory(directory);
        var models = FindModels();
        using var launcher = new ProcessEngineLauncher(
            FindEngine(),
            $"{pipeName} \"{Path.Combine(directory, "store")}\""
                + (models is null ? "" : $" \"{models}\""),
            stderrPath: Path.Combine(Path.GetTempPath(), "ambient-track-test.log"));
        using var host = new EngineSupervisor(
            launcher, new FakeSession(), TimeProvider.System,
            new FileCrashLog(Path.Combine(directory, "crashes.jsonl")));
        await using var connection = new EngineConnection(host, async (pid, ct) =>
            await PipeTransport.ConnectAsync(pipeName, Timeout, pid, ct));
        try
        {
            host.Start();
            await RetryAsync(() => connection.RequestAsync("engine/echo", new { payload = "up" }, Timeout));

            var noteReady = new TaskCompletionSource<string>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            var patientReady = new TaskCompletionSource<string>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            var partials = 0;
            connection.NotificationReceived += (method, parameters) =>
            {
                switch (method)
                {
                    case "note/partial":
                        partials++;
                        break;
                    case "note/ready":
                        noteReady.TrySetResult(parameters.GetProperty("text").GetString() ?? "");
                        break;
                    case "note/failed":
                        noteReady.TrySetException(new InvalidOperationException(
                            parameters.GetProperty("detail").GetString()));
                        break;
                    case "patient/ready":
                        patientReady.TrySetResult(
                            parameters.GetProperty("text").GetString() ?? "");
                        break;
                    case "patient/failed":
                        patientReady.TrySetException(new InvalidOperationException(
                            parameters.GetProperty("detail").GetString()));
                        break;
                    default:
                        break;
                }
            };

            await connection.RequestAsync(
                "session/start",
                new { replay = new { path = track, speed = 16.0, monitor = false } }, Timeout);
            await Task.Delay(TimeSpan.FromSeconds(20));  // ~5 min of audio at 16x
            var stop = await connection.RequestAsync(
                "session/stop", null, TimeSpan.FromSeconds(240));

            var id = stop.GetProperty("sessionId").GetString();
            var transcript = await connection.RequestAsync(
                "session/transcript", new { id }, Timeout);
            var labelled = transcript.GetProperty("turns").EnumerateArray()
                .Count(t => t.GetProperty("speaker").GetString() is "doctor" or "patient");
            Assert.True(labelled > 5, $"expected a labelled transcript, got {labelled} labelled turns");

            // The real note follows, streamed then stored
            var note = await noteReady.Task.WaitAsync(TimeSpan.FromSeconds(300));
            Assert.False(string.IsNullOrWhiteSpace(note));
            Assert.True(partials > 3, $"the note must stream, saw {partials} partials");
            var stored = await connection.RequestAsync("session/note", new { id }, Timeout);
            Assert.Equal(note, stored.GetProperty("text").GetString());

            // The patient sheet follows the note
            var patient = await patientReady.Task.WaitAsync(TimeSpan.FromSeconds(300));
            Assert.Contains("Your appointment today", patient);
            var storedPatient =
                await connection.RequestAsync("session/patient", new { id }, Timeout);
            Assert.Equal(patient, storedPatient.GetProperty("text").GetString());
        }
        finally
        {
            host.Shutdown();
            try
            {
                Directory.Delete(directory, recursive: true);
            }
            catch (IOException)
            {
            }
        }
    }

    [Fact]
    public async Task AKilledEngineResumesTheSessionAndTheConsultSurvives()
    {
        var track = Path.Combine(
            Path.GetDirectoryName(FindModels()) ?? "", "demo", "day2_consultation02_mixed.wav");
        if (!File.Exists(track))
        {
            return;  // demo tracks not staged on this machine
        }

        var pipeName = $"LOCAL\\ambient-resume-{Guid.NewGuid():N}";
        var directory = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        Directory.CreateDirectory(directory);
        var models = FindModels();
        using var launcher = new ProcessEngineLauncher(
            FindEngine(),
            $"{pipeName} \"{Path.Combine(directory, "store")}\""
                + (models is null ? "" : $" \"{models}\""),
            stderrPath: Path.Combine(Path.GetTempPath(), "ambient-resume-test.log"));
        using var host = new EngineSupervisor(
            launcher, new FakeSession(), TimeProvider.System,
            new FileCrashLog(Path.Combine(directory, "crashes.jsonl")));
        await using var connection = new EngineConnection(host, async (pid, ct) =>
            await PipeTransport.ConnectAsync(pipeName, Timeout, pid, ct));
        try
        {
            host.Start();
            await RetryAsync(() => connection.RequestAsync("engine/echo", new { payload = "up" }, Timeout));

            var noteReady = new TaskCompletionSource<string>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            connection.NotificationReceived += (method, parameters) =>
            {
                switch (method)
                {
                    case "note/ready":
                        noteReady.TrySetResult(parameters.GetProperty("text").GetString() ?? "");
                        break;
                    case "note/failed":
                        noteReady.TrySetException(new InvalidOperationException(
                            parameters.GetProperty("detail").GetString()));
                        break;
                    default:
                        break;
                }
            };

            var started = await connection.RequestAsync(
                "session/start",
                new { replay = new { path = track, speed = 16.0, monitor = false } }, Timeout);
            var firstId = started.GetProperty("sessionId").GetString();

            // Mid-consult, the engine dies the way the driver fault kills it
            await Task.Delay(TimeSpan.FromSeconds(8));
            Process.GetProcessById(host.EnginePid!.Value).Kill();
            await RetryAsync(() => connection.RequestAsync("engine/echo", new { payload = "back" }, Timeout));

            // The shell's resume: stored audio replays ahead of the rest of the file
            var resumed = await connection.RequestAsync(
                "session/start",
                new
                {
                    resume = firstId,
                    replay = new { path = track, speed = 16.0, monitor = false },
                }, TimeSpan.FromSeconds(60));
            var secondId = resumed.GetProperty("sessionId").GetString();
            Assert.NotEqual(firstId, secondId);

            await Task.Delay(TimeSpan.FromSeconds(14));
            var stop = await connection.RequestAsync("session/stop", null, TimeSpan.FromSeconds(240));
            Assert.Equal(secondId, stop.GetProperty("sessionId").GetString());

            // The transcript covers the whole consult, not just the tail
            var transcript = await connection.RequestAsync(
                "session/transcript", new { id = secondId }, Timeout);
            var labelled = transcript.GetProperty("turns").EnumerateArray()
                .Count(t => t.GetProperty("speaker").GetString() is "doctor" or "patient");
            Assert.True(labelled > 5, $"expected a full labelled transcript, got {labelled} turns");

            var note = await noteReady.Task.WaitAsync(TimeSpan.FromSeconds(300));
            Assert.False(string.IsNullOrWhiteSpace(note));
        }
        finally
        {
            host.Shutdown();
            try
            {
                Directory.Delete(directory, recursive: true);
            }
            catch (IOException)
            {
            }
        }
    }

    [Fact]
    public async Task LiveCountersSurviveAKilledEngineAndResume()
    {
        var track = Path.Combine(
            Path.GetDirectoryName(FindModels()) ?? "", "demo", "day2_consultation02_mixed.wav");
        if (!File.Exists(track))
        {
            return;
        }

        var pipeName = $"LOCAL\\ambient-counters-{Guid.NewGuid():N}";
        var directory = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        Directory.CreateDirectory(directory);
        var models = FindModels();
        using var launcher = new ProcessEngineLauncher(
            FindEngine(),
            $"{pipeName} \"{Path.Combine(directory, "store")}\""
                + (models is null ? "" : $" \"{models}\""),
            stderrPath: Path.Combine(Path.GetTempPath(), "ambient-counters-test.log"));
        using var host = new EngineSupervisor(
            launcher, new FakeSession(), TimeProvider.System,
            new FileCrashLog(Path.Combine(directory, "crashes.jsonl")));
        await using var connection = new EngineConnection(host, async (pid, ct) =>
            await PipeTransport.ConnectAsync(pipeName, Timeout, pid, ct));
        var session = new ConsultationViewModel(
            connection, new InlineDispatcher(), new TranscriptViewModel(), new NoteViewModel(),
            new StatusBarViewModel());
        try
        {
            host.Start();
            await RetryAsync(() => connection.RequestAsync("engine/echo", new { payload = "up" }, Timeout));

            await session.StartRecordingAsync(new ReplayRequest(track, 16.0, false));
            Assert.Equal(SessionState.Recording, session.State);
            await WaitUntilAsync(() => session.AudioSeconds > 3, TimeSpan.FromSeconds(20));

            // Two kills in quick succession, the second mid-resume - the
            // double-crash sequence observed in the field
            var atKill = session.AudioSeconds;
            var firstPid = host.EnginePid!.Value;
            Process.GetProcessById(firstPid).Kill();
            await WaitUntilAsync(
                () => host.EnginePid is int pid && pid != firstPid, TimeSpan.FromSeconds(30));
            Process.GetProcessById(host.EnginePid!.Value).Kill();

            // The supervisor restarts again, the resume retries, and the
            // counters must keep counting from where they were
            await WaitUntilAsync(
                () => session.AudioSeconds > atKill + 30, TimeSpan.FromSeconds(90));
            Assert.Equal(SessionState.Recording, session.State);

            await session.StopRecordingAsync();
            Assert.True(
                session.State is SessionState.Finalising or SessionState.Review,
                $"stop left the session in {session.State}");
        }
        finally
        {
            host.Shutdown();
            try
            {
                Directory.Delete(directory, recursive: true);
            }
            catch (IOException)
            {
            }
        }
    }

    [Fact]
    [Trait("Requires", "CrashBattery")]
    public async Task AcceleratedSessionStartsSurviveTheWorkerLoad()
    {
        var track = Path.Combine(
            Path.GetDirectoryName(FindModels()) ?? "", "demo", "day2_consultation02_mixed.wav");
        if (!File.Exists(track))
        {
            return;
        }

        // A cold compile cache is the hard case: the 9B compiles on the GPU
        // while 16x whisper floods it, the collision seen in the field
        var noteCache = Path.Combine(FindModels()!, "qwen3.5-9b-int4", ".cache");
        var crashes = 0;
        for (var round = 0; round < 6; round++)
        {
            if (Directory.Exists(noteCache))
            {
                try
                {
                    Directory.Delete(noteCache, recursive: true);
                }
                catch (IOException)
                {
                }
            }

            var pipeName = $"LOCAL\\ambient-battery-{Guid.NewGuid():N}";
            var directory = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
            Directory.CreateDirectory(directory);
            var crashLog = Path.Combine(directory, "crashes.jsonl");
            using var launcher = new ProcessEngineLauncher(
                FindEngine(),
                $"{pipeName} \"{Path.Combine(directory, "store")}\" \"{FindModels()}\"",
                stderrPath: Path.Combine(Path.GetTempPath(), "ambient-battery-test.log"));
            using var host = new EngineSupervisor(
                launcher, new FakeSession(), TimeProvider.System, new FileCrashLog(crashLog));
            await using var connection = new EngineConnection(host, async (pid, ct) =>
                await PipeTransport.ConnectAsync(pipeName, Timeout, pid, ct));
            try
            {
                host.Start();
                await RetryAsync(() => connection.RequestAsync("engine/echo", new { payload = "up" }, Timeout));
                await connection.RequestAsync(
                    "session/start",
                    new { replay = new { path = track, speed = 16.0, monitor = false } }, Timeout);
                await Task.Delay(TimeSpan.FromSeconds(12));
                if (File.Exists(crashLog) && File.ReadAllLines(crashLog).Length > 0)
                {
                    crashes++;
                }
                else
                {
                    await connection.RequestAsync("session/cancel", null, TimeSpan.FromSeconds(30));
                }
            }
            finally
            {
                host.Shutdown();
                try
                {
                    Directory.Delete(directory, recursive: true);
                }
                catch (IOException)
                {
                }
            }
        }

        Assert.True(crashes == 0, $"{crashes}/6 accelerated session starts crashed the engine");
    }

    private static async Task WaitUntilAsync(Func<bool> condition, TimeSpan limit)
    {
        var deadline = DateTime.UtcNow + limit;
        while (!condition())
        {
            Assert.True(DateTime.UtcNow < deadline, "condition not reached in time");
            await Task.Delay(100);
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

    private static string FindEngine() => EnginePath.Find();

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
