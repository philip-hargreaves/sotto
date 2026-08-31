using System.Collections.ObjectModel;
using System.Text.Json;
using CommunityToolkit.Mvvm.ComponentModel;
using Sotto.App.Core.Hosting;
using Sotto.Client;

namespace Sotto.App.Core.ViewModels;

/// <summary>One bar of the level history; mutated in place, never replaced.</summary>
public sealed partial class LevelBar : ObservableObject
{
    [ObservableProperty]
    public partial double Height { get; set; } = 2;
}

public sealed partial class StatusBarViewModel : ObservableObject
{
    private const int MaxLogEntries = 200;
    private const int MeterBars = 40;

    /// <summary>Rolling RMS history; fixed objects so layout never thrashes.</summary>
    public ObservableCollection<LevelBar> Meter { get; } =
        [.. Enumerable.Range(0, MeterBars).Select(_ => new LevelBar())];

    // Clinician-facing: no engine, model or process vocabulary
    [ObservableProperty]
    public partial string EngineStateLabel { get; set; } = "Starting";

    [ObservableProperty]
    public partial string LatestActivity { get; private set; } = "";

    // One status on screen, replaced as things happen: abnormal readiness
    // outranks activity, activity outranks Ready; Busy drives the one ring
    public string DisplayLabel =>
        !_ready || _status != EngineStatus.Running ? EngineStateLabel
        : LatestActivity.Length > 0 ? LatestActivity
        : EngineStateLabel;

    public bool Busy => EngineStarting || _activityBusy;

    private bool _activityBusy;

    [ObservableProperty]
    public partial string PerformanceLine { get; set; } = "";

    private readonly IEngineClient? _engine;
    private readonly TimeProvider _time = TimeProvider.System;
    private readonly ThroughputMeter _meter = new();
    private readonly long _started;

    public StatusBarViewModel()
    {
    }

    /// <summary>
    /// With an engine, the bar meters generation live: one partial per token
    /// from whichever lane streams, so the number moves with every token.
    /// </summary>
    public StatusBarViewModel(IEngineClient engine, IUiDispatcher dispatcher,
        TimeProvider? time = null)
    {
        _engine = engine;
        _time = time ?? TimeProvider.System;
        _started = _time.GetTimestamp();
        engine.NotificationReceived += (method, _) => dispatcher.Post(() =>
        {
            switch (method)
            {
                case "note/partial" or "patient/partial" or "translate/partial":
                    _meter.Token(Now());
                    PublishThroughput();
                    break;
                case "note/ready" or "note/failed" or "patient/ready" or "patient/failed"
                    or "translate/ready" or "translate/failed":
                    _meter.End(Now());
                    PublishThroughput();
                    break;
                default:
                    break;
            }
        });
    }

    private double Now() => _time.GetElapsedTime(_started).TotalSeconds;

    private void PublishThroughput()
    {
        TokensPerSecond = _meter.TokensPerSecond(Now());
        TokensStreaming = _meter.Streaming;
    }

    /// <summary>Rolling tokens per second; holds its last value after a stream ends.</summary>
    [ObservableProperty]
    public partial double TokensPerSecond { get; private set; }

    [ObservableProperty]
    public partial bool TokensStreaming { get; private set; }

    /// <summary>A new consultation meters from nothing.</summary>
    public void ResetThroughput()
    {
        _meter.Reset();
        PublishThroughput();
    }

    /// <summary>Transcription speed as a multiple of real time; 0 when unknown.</summary>
    [ObservableProperty]
    public partial double RealtimeFactor { get; private set; }

    /// <summary>Amber: transcription is barely keeping up with the room.</summary>
    public bool RealtimeLow => MicVisible && RealtimeFactor > 0 && RealtimeFactor < 2;

    // Polled at 1 Hz while recording - the factor updates per decoded
    // window, so that IS its native rate. Failures leave the last value.
    public async Task PollMetricsOnceAsync()
    {
        if (_engine is null || !_engine.Connected)
        {
            return;
        }

        try
        {
            var metrics = await _engine
                .RequestAsync("engine/metrics", null, TimeSpan.FromSeconds(2))
                .ConfigureAwait(true);
            if (metrics.TryGetProperty("asrRealtimeFactor", out var factor)
                && factor.ValueKind == JsonValueKind.Number)
            {
                RealtimeFactor = factor.GetDouble();
            }
        }
        catch (Exception)
        {
        }
    }

    partial void OnRealtimeFactorChanged(double value) =>
        OnPropertyChanged(nameof(RealtimeLow));

    [ObservableProperty]
    public partial double MicLevel { get; private set; }

    [ObservableProperty]
    public partial bool MicClipped { get; private set; }

    /// <summary>The level bar shows only while audio is flowing.</summary>
    [ObservableProperty]
    public partial bool MicVisible { get; private set; }

    public ObservableCollection<string> LogEntries { get; } = [];

    public void SetMicLevel(double level, bool clipped)
    {
        MicLevel = level;
        MicClipped = clipped;
        for (var i = 0; i < Meter.Count - 1; i++)
        {
            Meter[i].Height = Meter[i + 1].Height;
        }

        Meter[^1].Height = 2 + Math.Clamp(level, 0, 1) * 26;
    }

    public void SetMicVisible(bool visible)
    {
        MicVisible = visible;
        OnPropertyChanged(nameof(RealtimeLow));
        if (!visible)
        {
            SetMicLevel(0, false);
            return;
        }

        if (_engine is not null && !_polling)
        {
            _ = PollWhileRecordingAsync();
        }
    }

    private bool _polling;

    private async Task PollWhileRecordingAsync()
    {
        _polling = true;
        try
        {
            while (MicVisible)
            {
                await PollMetricsOnceAsync().ConfigureAwait(true);
                await Task.Delay(TimeSpan.FromSeconds(1), _time).ConfigureAwait(true);
            }
        }
        finally
        {
            _polling = false;
        }
    }

    private EngineStatus _status = EngineStatus.Stopped;
    private EngineFault? _fault;
    private bool _ready;

    /// <summary>True in every transient state; the status ring spins on it.</summary>
    [ObservableProperty]
    public partial bool EngineStarting { get; private set; }

    public void SetEngineState(EngineStatus status, EngineFault? fault)
    {
        _status = status;
        _fault = fault;
        Recompute();

        // Silent restarts stay out of the activity log; faults go in
        if (status == EngineStatus.Faulted)
        {
            Append(EngineStateLabel);
        }
    }

    public void SetEngineReady(bool ready)
    {
        _ready = ready;
        Recompute();
    }

    private void Recompute()
    {
        EngineStarting = _status == EngineStatus.Running && !_ready
            || _status == EngineStatus.Restarting;
        EngineStateLabel = _status switch
        {
            EngineStatus.Running when !_ready => "Setting up",
            EngineStatus.Running => "Ready",
            EngineStatus.Restarting => "Recovering",
            EngineStatus.Faulted => _fault?.Kind switch
            {
                EngineFaultKind.SessionInterrupted =>
                    "A problem interrupted the consultation - recovering",
                _ => "Recording is unavailable - please restart the app",
            },
            _ => "Not running",
        };
        OnPropertyChanged(nameof(DisplayLabel));
        OnPropertyChanged(nameof(Busy));
    }

    /// <summary>Log-only detail; the displayed status stays concise.</summary>
    public void Log(string line)
    {
        LogEntries.Add(line);
        while (LogEntries.Count > MaxLogEntries)
        {
            LogEntries.RemoveAt(0);
        }
    }

    public void Append(string line, bool busy = false)
    {
        LogEntries.Add(line);
        while (LogEntries.Count > MaxLogEntries)
        {
            LogEntries.RemoveAt(0);
        }

        _activityBusy = busy;
        LatestActivity = line;
        OnPropertyChanged(nameof(DisplayLabel));
        OnPropertyChanged(nameof(Busy));
    }
}
