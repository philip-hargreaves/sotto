using System.Diagnostics;
using System.Text.Json;
using Sotto.Client;

namespace Sotto.App.Core.Metrics;

/// <summary>
/// Appends one JSON line per finished session to metrics.jsonl: the engine's
/// metrics snapshot plus shell-side note latency and memory peaks. Numbers
/// and device names only, never content. Writes nothing unless enabled.
/// </summary>
public sealed class PerformanceCollector(
    IEngineClient engine, Func<bool> enabled, Func<int?> enginePid, string path)
{
    private static readonly JsonSerializerOptions Json = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull,
    };

    private DateTimeOffset _start;
    private string _source = "";
    private double _replaySpeed;
    private string? _track;
    private long? _availableAtStartMb;
    private Stopwatch? _stopClock;
    private double? _firstPartialSeconds;

    public string Path { get; } = path;

    public void SessionStarted(string source, double replaySpeed, string? track)
    {
        if (!enabled())
        {
            _stopClock = null;
            return;
        }

        _start = DateTimeOffset.UtcNow;
        _source = source;
        _replaySpeed = replaySpeed;
        _track = track;
        _availableAtStartMb = AvailableMemoryMb();
        _stopClock = null;
        _firstPartialSeconds = null;
    }

    public void StopRequested()
    {
        _stopClock ??= Stopwatch.StartNew();
    }

    public void NotePartial()
    {
        if (_stopClock is not null)
        {
            _firstPartialSeconds ??= _stopClock.Elapsed.TotalSeconds;
        }
    }

    /// <summary>Fetches the engine snapshot and appends the session's line.</summary>
    public async Task SessionFinishedAsync(string? noteFailure, int noteChars)
    {
        if (!enabled() || _stopClock is null)
        {
            return;
        }

        var stopClock = _stopClock;
        _stopClock = null;
        JsonElement? engineMetrics = null;
        try
        {
            engineMetrics = await engine
                .RequestAsync("engine/metrics", null, TimeSpan.FromSeconds(5))
                .ConfigureAwait(false);
        }
        catch (Exception)
        {
        }

        var record = new
        {
            schema = 1,
            start = _start,
            source = _source,
            replaySpeed = _replaySpeed > 0 ? (double?)_replaySpeed : null,
            track = _track,
            engine = engineMetrics,
            note = new
            {
                firstPartialAfterStopSeconds = Round(_firstPartialSeconds),
                readyAfterStopSeconds = Round(stopClock.Elapsed.TotalSeconds),
                chars = noteChars,
                failed = noteFailure,
            },
            // The power situation at stop: it decides the finalise floor
            power = PowerState.Read(),
            memory = new
            {
                availableAtStartMb = _availableAtStartMb,
                peakWorkingSetMb = EngineMemoryMb(p => p.PeakWorkingSet64),
                peakCommitMb = EngineMemoryMb(p => p.PeakPagedMemorySize64),
            },
        };

        try
        {
            Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Path)!);
            File.AppendAllText(Path, JsonSerializer.Serialize(record, Json) + Environment.NewLine);
        }
        catch (IOException)
        {
        }
    }

    private static double? Round(double? seconds) =>
        seconds is null ? null : Math.Round(seconds.Value, 2);

    private long? EngineMemoryMb(Func<Process, long> metric)
    {
        try
        {
            var pid = enginePid();
            if (pid is null)
            {
                return null;
            }

            using var process = Process.GetProcessById(pid.Value);
            return metric(process) / (1024 * 1024);
        }
        catch (Exception)
        {
            return null;
        }
    }

    private static long? AvailableMemoryMb()
    {
        try
        {
            var info = GC.GetGCMemoryInfo();
            return (info.TotalAvailableMemoryBytes - info.MemoryLoadBytes) / (1024 * 1024);
        }
        catch (Exception)
        {
            return null;
        }
    }
}
