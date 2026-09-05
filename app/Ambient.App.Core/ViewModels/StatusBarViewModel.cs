using System.Collections.ObjectModel;
using System.Text.Json;
using CommunityToolkit.Mvvm.ComponentModel;
using Ambient.App.Core.Hosting;
using Ambient.Client;

namespace Ambient.App.Core.ViewModels;

public sealed partial class StatusBarViewModel : ObservableObject
{
    private const int MaxLogEntries = 200;

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

    // The two models a clinician's machine actually works for, each with its
    // live number: "Whisper Turbo · GPU · 33× RT", "Qwen3.5 9B · GPU · 14.2 tok/s"
    [ObservableProperty]
    public partial string AsrChip { get; private set; } = "";

    [ObservableProperty]
    public partial string NoteChip { get; private set; } = "";

    private string _asrName = "";
    private string _noteName = "";
    private string _asrDevice = "";
    private string _noteDevice = "";

    /// <summary>Fallback when a manifest has no display name: "whisper-turbo-int8"
    /// reads as "Whisper Turbo"; precision suffix dropped, size tokens kept.</summary>
    public static string FriendlyModelName(string id)
    {
        var words = id.Split('-')
            .Where(t => t is not ("int8" or "int4" or "fp16" or "fp32"))
            .Select(t => System.Text.RegularExpressions.Regex.IsMatch(t, @"^\d+b$")
                ? t.ToUpperInvariant()
                : char.ToUpperInvariant(t[0]) + t[1..]);
        return string.Join(' ', words);
    }

    private static string ShortDevice(string device) =>
        device.Split('.')[0];  // "GPU.0" is a build detail; "GPU" is the fact

    private async Task LoadModelsAsync()
    {
        if (_engine is null || !_engine.Connected)
        {
            return;
        }

        try
        {
            var response = await _engine
                .RequestAsync("engine/models", null, TimeSpan.FromSeconds(5))
                .ConfigureAwait(true);
            // The default tier is what the engine loads; an ablation export
            // beside it must not name the chip
            foreach (var model in response.GetProperty("models").EnumerateArray()
                         .OrderBy(m => m.GetProperty("tier").GetString() == "default" ? 0 : 1))
            {
                var task = model.GetProperty("task").GetString();
                var id = model.GetProperty("id").GetString() ?? "";
                var name = model.TryGetProperty("name", out var given)
                           && !string.IsNullOrWhiteSpace(given.GetString())
                    ? given.GetString()!
                    : FriendlyModelName(id);
                var device = ShortDevice(model.GetProperty("device").GetString() ?? "");
                if (task == "asr" && _asrName.Length == 0)
                {
                    (_asrName, _asrDevice) = (name, device);
                }
                else if (task == "note" && _noteName.Length == 0)
                {
                    (_noteName, _noteDevice) = (name, device);
                }
            }
        }
        catch (Exception)
        {
        }

        RecomputeChips();
        await PollMetricsOnceAsync().ConfigureAwait(true);  // actual devices beat manifests
    }

    /// <summary>The chip's dot: green while this model is working right now.</summary>
    public bool AsrActive => MicVisible || DecodeActive;

    public bool NoteActive => TokensStreaming;

    // The resting dot and healthy text are the visible-inverse halves of the
    // colour pairs the view swaps; XAML gets properties, never functions
    public bool AsrResting => !AsrActive;

    public bool NoteResting => !NoteActive;

    public bool RealtimeHealthy => !RealtimeLow;

    // Live figures are unlabelled and move; settled ones say "Averaged" - the
    // session's true average, held through review for reading after a run
    private void RecomputeChips()
    {
        OnPropertyChanged(nameof(AsrActive));
        OnPropertyChanged(nameof(NoteActive));
        OnPropertyChanged(nameof(AsrResting));
        OnPropertyChanged(nameof(NoteResting));
        OnPropertyChanged(nameof(RealtimeHealthy));
        AsrChip = Chip(_asrName, _asrDevice,
            (MicVisible || DecodeActive) && RealtimeFactor > 0
                ? $"{Figure(RealtimeFactor)}× RT"
                : _frozenRealtime > 0 ? $"Averaged {Figure(_frozenRealtime)}× RT" : "");
        var tok = TokensPerSecond.ToString("0.0", System.Globalization.CultureInfo.CurrentCulture);
        NoteChip = Chip(_noteName, _noteDevice,
            TokensPerSecond <= 0 ? ""
            : TokensStreaming ? $"{tok} tok/s"
            : $"Averaged {tok} tok/s");

        static string Figure(double value) => value.ToString(
            value < 10 ? "0.0" : "0", System.Globalization.CultureInfo.CurrentCulture);

        static string Chip(string name, string device, string figure)
        {
            if (name.Length == 0)
            {
                return "";
            }

            var chip = device.Length > 0 ? $"{name} · {device}" : name;
            return figure.Length > 0 ? $"{chip} · {figure}" : chip;
        }
    }

    private readonly IEngineClient? _engine;
    private readonly TimeProvider _time = TimeProvider.System;
    private readonly ThroughputMeter _meter = new();
    private readonly Func<double> _memoryGb = ReadMemoryGb;
    private readonly long _started;

    public StatusBarViewModel()
    {
    }

    /// <summary>
    /// With an engine, the bar meters generation live: one partial per token
    /// from whichever lane streams, so the number moves with every token.
    /// </summary>
    public StatusBarViewModel(IEngineClient engine, IUiDispatcher dispatcher,
        TimeProvider? time = null, Func<double>? memoryGb = null)
    {
        _engine = engine;
        _time = time ?? TimeProvider.System;
        _memoryGb = memoryGb ?? ReadMemoryGb;
        _started = _time.GetTimestamp();
        engine.ConnectedChanged += connected => dispatcher.Post(() =>
        {
            if (connected)
            {
                _ = LoadModelsAsync();
                StartPolling();
            }
        });
        if (engine.Connected)
        {
            _ = LoadModelsAsync();
            StartPolling();
        }

        engine.NotificationReceived += (method, parameters) => dispatcher.Post(() =>
        {
            switch (method)
            {
                case "note/partial" or "patient/partial" or "translate/partial":
                    _meter.Token(Now());
                    PublishThroughput(SourceRate(parameters));
                    break;
                case "note/ready" or "patient/ready" or "translate/ready":
                    _meter.End(Now());
                    // The ready event carries the whole-generation average
                    PublishThroughput(SourceRate(parameters));
                    break;
                case "note/failed" or "patient/failed" or "translate/failed":
                    _meter.End(Now());
                    PublishThroughput(null);
                    break;
                default:
                    break;
            }
        });
    }

    private double Now() => _time.GetElapsedTime(_started).TotalSeconds;

    // The engine measures at the source, before its notification throttle,
    // so its figure beats the local arrival count whenever it is present
    private static double? SourceRate(JsonElement parameters) =>
        parameters.ValueKind == JsonValueKind.Object
            && parameters.TryGetProperty("tokensPerSecond", out var rate)
            && rate.ValueKind == JsonValueKind.Number
        ? rate.GetDouble()
        : null;

    private void PublishThroughput(double? sourceRate = null)
    {
        TokensPerSecond = sourceRate ?? _meter.TokensPerSecond(Now());
        TokensStreaming = _meter.Streaming;
        RecomputeChips();
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
        _frozenRealtime = 0;
        PublishThroughput();
    }

    /// <summary>Transcription speed as a multiple of real time; 0 when unknown.</summary>
    [ObservableProperty]
    public partial double RealtimeFactor { get; private set; }

    /// <summary>
    /// True from stop until the sealed transcript loads: the finalise tail
    /// decode - the NPU's longest stage - keeps the RT figure on screen.
    /// </summary>
    [ObservableProperty]
    public partial bool DecodeActive { get; private set; }

    private double _frozenRealtime;

    public void SetDecodeActive(bool active)
    {
        if (DecodeActive && !active && RealtimeFactor > 0)
        {
            // Sealed: the per-session counters make this the session's
            // exact average decode speed, held for reading after the run
            _frozenRealtime = RealtimeFactor;
        }

        DecodeActive = active;
        OnPropertyChanged(nameof(RealtimeLow));
        RecomputeChips();
    }

    /// <summary>Amber: transcription is barely keeping up with the room.</summary>
    public bool RealtimeLow =>
        (MicVisible || DecodeActive) && RealtimeFactor > 0 && RealtimeFactor < 2;

    /// <summary>"Memory · 5.1 GB": the product's whole working set.</summary>
    [ObservableProperty]
    public partial string MemoryChip { get; private set; } = "";

    /// <summary>The chips are for testing, not GPs: off unless opted in.</summary>
    [ObservableProperty]
    public partial bool MetricsVisible { get; set; }

    // Shell + engine + note host: the honest on-device footprint. By name
    // because the note host is the engine's child, not the shell's
    private static double ReadMemoryGb()
    {
        try
        {
            var bytes = Environment.WorkingSet;
            foreach (var name in new[] { "ambient_engine", "ambient_note_host" })
            {
                foreach (var process in System.Diagnostics.Process.GetProcessesByName(name))
                {
                    using (process)
                    {
                        bytes += process.WorkingSet64;
                    }
                }
            }

            return bytes / (1024.0 * 1024 * 1024);
        }
        catch (Exception)
        {
            return 0;
        }
    }

    // Polled at 1 Hz while recording - the factor updates per decoded
    // window, so that IS its native rate. Failures leave the last value.
    public async Task PollMetricsOnceAsync()
    {
        var memory = _memoryGb();
        MemoryChip = memory > 0
            ? $"Memory · {memory.ToString("0.0", System.Globalization.CultureInfo.CurrentCulture)} GB"
            : "";

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

            if (metrics.TryGetProperty("devices", out var devices)
                && devices.ValueKind == JsonValueKind.Object)
            {
                if (devices.TryGetProperty("asr", out var asr))
                {
                    _asrDevice = ShortDevice(asr.GetString() ?? _asrDevice);
                }

                if (devices.TryGetProperty("note", out var note))
                {
                    _noteDevice = ShortDevice(note.GetString() ?? _noteDevice);
                }

                RecomputeChips();
            }
        }
        catch (Exception)
        {
        }
    }

    partial void OnRealtimeFactorChanged(double value)
    {
        OnPropertyChanged(nameof(RealtimeLow));
        RecomputeChips();
    }

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
    }

    public void SetMicVisible(bool visible)
    {
        MicVisible = visible;
        OnPropertyChanged(nameof(RealtimeLow));
        RecomputeChips();
        if (!visible)
        {
            SetMicLevel(0, false);
        }
    }

    private bool _polling;

    private void StartPolling()
    {
        if (_engine is not null && !_polling)
        {
            _ = PollWhileConnectedAsync();
        }
    }

    // One loop for the connected lifetime: 1 Hz while recording, 5 s idle,
    // so a device switch reaches the chip within seconds
    private async Task PollWhileConnectedAsync()
    {
        _polling = true;
        try
        {
            while (_engine!.Connected)
            {
                await PollMetricsOnceAsync().ConfigureAwait(true);
                await Task.Delay(TimeSpan.FromSeconds(MicVisible || DecodeActive ? 1 : 5), _time)
                    .ConfigureAwait(true);
            }
        }
        finally
        {
            _polling = false;  // reconnection starts a fresh loop
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
