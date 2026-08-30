using System.Text.Json;
using CommunityToolkit.Mvvm.ComponentModel;
using Sotto.App.Core.Hosting;
using Sotto.Client;

namespace Sotto.App.Core.ViewModels;

/// <summary>A file replayed as the session's audio source.</summary>
public sealed record ReplayRequest(string Path, double Speed, bool Monitor);

/// <summary>
/// Owns the session lifecycle.
/// </summary>
public sealed partial class ConsultationViewModel : ObservableObject, ISessionState
{
    private static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(5);

    // Accelerated replay legitimately leaves a decode backlog for stop to
    // drain; at 1x this is seconds
    private static readonly TimeSpan StopTimeout = TimeSpan.FromSeconds(180);

    private readonly IEngineClient _engine;
    private readonly Metrics.PerformanceCollector? _metrics;
    private readonly AppPreferences? _preferences;

    [ObservableProperty]
    public partial SessionState State { get; private set; } = SessionState.Idle;

    /// <summary>False while the engine is still starting or reconnecting.</summary>
    [ObservableProperty]
    public partial bool EngineReady { get; private set; }

    [ObservableProperty]
    public partial bool Paused { get; private set; }

    // Delivered-audio position: one level event per 100 ms of audio, at any
    // replay speed
    [ObservableProperty]
    public partial double AudioSeconds { get; private set; }

    /// <summary>The active session's replay request; null for a microphone.</summary>
    [ObservableProperty]
    public partial ReplayRequest? ActiveReplay { get; private set; }

    /// <summary>
    /// True once the sealed transcript has been fetched; the panes open on
    /// this, never on state alone - a pane with no transcript is worse than
    /// the centred spinner it would replace.
    /// </summary>
    /// <summary>
    /// Where a stop has got to. The centre stage holds until the note streams,
    /// so the pause of the note prefill is spent on a spinner that says so,
    /// not on an empty document. Advances only forwards within one stop.
    /// </summary>
    [ObservableProperty]
    public partial FinalisePhase Phase { get; private set; } = FinalisePhase.None;

    /// <summary>
    /// False only during first-time setup, while the one-off model compiles
    /// run: recording is blocked so nobody's first impression is the slow
    /// path. Warm launches are never gated.
    /// </summary>
    [ObservableProperty]
    public partial bool ModelsReady { get; private set; } = true;

    public bool ConsultationActive => State != SessionState.Idle;

    public TranscriptViewModel Transcript { get; }

    public NoteViewModel Note { get; }

    public StatusBarViewModel Status { get; }

    public ConsultationViewModel(
        IEngineClient engine, IUiDispatcher dispatcher,
        TranscriptViewModel transcript, NoteViewModel note, StatusBarViewModel status,
        Metrics.PerformanceCollector? metrics = null, TimeSpan? readinessPollInterval = null,
        AppPreferences? preferences = null)
    {
        _engine = engine;
        _metrics = metrics;
        _preferences = preferences;
        _readinessPollInterval = readinessPollInterval ?? TimeSpan.FromSeconds(2);
        Transcript = transcript;
        Note = note;
        Status = status;
        EngineReady = engine.Connected;
        Note.TranslateRequested = TranslateAsync;
        Note.RegenerateRequested = RegenerateNoteAsync;
        Note.SaveNoteRequested = SaveNoteAsync;
        Note.SavePatientRequested = SavePatientAsync;
        // Persisted options applied before the change callback is wired,
        // so restoring them is not itself a change
        if (preferences is not null)
        {
            Note.Style = preferences.NoteStyle;
            Note.Detail = preferences.NoteDetail;
        }

        Note.OptionsChanged = OnNoteOptionsChanged;
        _engine.NotificationReceived +=
            (method, parameters) => dispatcher.Post(() => HandleNotification(method, parameters));
        // The status-bar label carries readiness; only the loss is log-worthy
        _engine.ConnectedChanged += connected => dispatcher.Post(() =>
        {
            EngineReady = connected;
            Status.SetEngineReady(connected);
            if (!connected)
            {
                // A restarted engine will never answer the in-flight translation
                Note.TranslationRunning = false;
                Status.Append("Connection lost - recovering", busy: true);
            }
            else
            {
                _ = LoadLanguagesAsync();
                _ = CheckReadinessAsync();
                _ = PushNoteOptionsAsync();
                if (State == SessionState.Recording)
                {
                    _ = ResumeAfterRestartAsync();
                }
            }
        });
        if (EngineReady)
        {
            _ = LoadLanguagesAsync();
            _ = CheckReadinessAsync();
            _ = PushNoteOptionsAsync();
        }
    }

    private readonly TimeSpan _readinessPollInterval;

    // First launch only: poll until the one-off compiles finish, then never
    // again. Fails open - a readiness error must not brick recording.
    private async Task CheckReadinessAsync()
    {
        try
        {
            var response = await _engine
                .RequestAsync("engine/readiness", null, RequestTimeout)
                .ConfigureAwait(true);
            if (!response.TryGetProperty("firstUse", out var f) || !f.GetBoolean())
            {
                ModelsReady = true;
                return;
            }

            while (!response.GetProperty("ready").GetBoolean())
            {
                if (ModelsReady)
                {
                    ModelsReady = false;
                    Status.Append("First-time setup - this can take a few minutes", busy: true);
                }

                await Task.Delay(_readinessPollInterval).ConfigureAwait(true);
                response = await _engine
                    .RequestAsync("engine/readiness", null, RequestTimeout)
                    .ConfigureAwait(true);
            }

            if (!ModelsReady)
            {
                ModelsReady = true;
                Status.Append("Ready");
            }
        }
        catch (Exception)
        {
            ModelsReady = true;
        }
    }

    // Empty when the engine ships without a translation model
    private async Task LoadLanguagesAsync()
    {
        try
        {
            var response = await _engine
                .RequestAsync("translate/languages", null, RequestTimeout)
                .ConfigureAwait(true);
            Note.Languages.Clear();
            foreach (var language in response.GetProperty("languages").EnumerateArray())
            {
                Note.Languages.Add(language.GetString() ?? "");
            }
        }
        catch (Exception)
        {
        }
    }

    public async Task TranslateAsync(string language)
    {
        if (_finalisedSessionId is null)
        {
            return;
        }

        Note.TranslationText = "";
        await RequestAsync("patient/translate", new { id = _finalisedSessionId, language })
            .ConfigureAwait(true);
    }

    private string? _finalisedSessionId;
    private string? _recordingSessionId;

    // True from a regenerate request until its pipeline settles; keeps the
    // rewrite out of the per-session metrics
    private bool _regenerating;

    private void OnNoteOptionsChanged()
    {
        if (_preferences is not null)
        {
            _preferences.NoteStyle = Note.Style;
            _preferences.NoteDetail = Note.Detail;
            _preferences.Save();
        }

        _ = PushNoteOptionsAsync();
    }

    // The engine holds options per process; a restart loses them
    private async Task PushNoteOptionsAsync()
    {
        if (_engine.Connected)
        {
            await RequestAsync("note/options", new { style = Note.Style, detail = Note.Detail })
                .ConfigureAwait(true);
        }
    }

    public async Task RegenerateNoteAsync()
    {
        if (State != SessionState.Review)
        {
            return;
        }

        var accepted = await RequestAsync(
            "note/regenerate", new { style = Note.Style, detail = Note.Detail })
            .ConfigureAwait(true);
        if (accepted)
        {
            _regenerating = true;
            Note.BeginRegenerate();
        }
    }

    public async Task SaveNoteAsync()
    {
        if (_finalisedSessionId is null)
        {
            return;
        }

        var saved = await RequestAsync(
            "note/update", new { id = _finalisedSessionId, text = Note.ClinicalNoteText })
            .ConfigureAwait(true);
        if (saved)
        {
            Status.Append("Note saved");
        }
    }

    public async Task SavePatientAsync()
    {
        if (_finalisedSessionId is null)
        {
            return;
        }

        var saved = await RequestAsync(
            "patient/update", new { id = _finalisedSessionId, text = Note.PatientInfoText })
            .ConfigureAwait(true);
        if (saved)
        {
            Status.Append("Patient note saved");
        }
    }

    // A restarted engine lost the live session, but its audio is stored:
    // resume replays it into a fresh session and recording carries on
    private async Task ResumeAfterRestartAsync()
    {
        var resume = _recordingSessionId;
        if (resume is null)
        {
            return;
        }

        try
        {
            var replay = ActiveReplay;
            var parameters = replay is null
                ? (object)new { resume }
                : new { resume, replay = new { path = replay.Path, speed = replay.Speed, monitor = replay.Monitor } };
            // A post-crash resume decrypts the stored audio on a recovering
            // machine; the default timeout is far too tight for it. The raw
            // request is used so that a lost engine is told apart from an
            // engine that answered - the swallowing wrapper cannot
            var response = await _engine.RequestAsync(
                "session/start", parameters, TimeSpan.FromSeconds(60)).ConfigureAwait(true);
            _recordingSessionId = response.TryGetProperty("sessionId", out var id)
                ? id.GetString() : null;
            Status.Append("Recording");
        }
        catch (Exception e) when (e is EngineErrorException or OperationCanceledException)
        {
            // The engine is up and cannot resume, or never answered
            State = SessionState.Idle;
            Status.SetMicVisible(false);
            Status.Append("Could not resume - session kept");
            Status.Log($"session/start failed: {e.Message}");
        }
        catch (Exception)
        {
            // The engine died again mid-resume; the session stays Recording so
            // the next reconnect retries, and the UI must not claim a session
            // that is not running
            Status.Append("Recovering", busy: true);
        }
    }

    public async Task StartRecordingAsync(ReplayRequest? replay = null)
    {
        if (State != SessionState.Idle)
        {
            return;
        }

        var parameters = replay is null
            ? null
            : new { replay = new { path = replay.Path, speed = replay.Speed, monitor = replay.Monitor } };
        var response = await RequestValueAsync("session/start", null, parameters).ConfigureAwait(true);
        if (response is null)
        {
            return;
        }

        _recordingSessionId = response.Value.TryGetProperty("sessionId", out var id)
            ? id.GetString() : null;
        Paused = false;
        AudioSeconds = 0;
        Phase = FinalisePhase.None;
        ActiveReplay = replay;
        State = SessionState.Recording;
        Status.SetMicVisible(true);
        Status.Append(replay is null ? "Recording" : "Replaying");
        _metrics?.SessionStarted(
            replay is null ? "mic" : "replay", replay?.Speed ?? 0,
            replay is null ? null : Path.GetFileNameWithoutExtension(replay.Path));
    }

    public async Task SetPausedAsync(bool paused)
    {
        if (State != SessionState.Recording || paused == Paused)
        {
            return;
        }

        if (await RequestAsync("session/pause", new { paused }).ConfigureAwait(true))
        {
            Paused = paused;
        }
    }

    public async Task SetMonitorAsync(bool on)
    {
        if (State == SessionState.Recording)
        {
            await RequestAsync("session/monitor", new { on }).ConfigureAwait(true);
        }
    }

    public async Task StopRecordingAsync()
    {
        if (State != SessionState.Recording)
        {
            return;
        }

        State = SessionState.Finalising;
        Phase = FinalisePhase.Sealing;
        Paused = false;
        ActiveReplay = null;
        _metrics?.StopRequested();
        Status.SetMicVisible(false);
        Note.Apply(NotePipelineEvent.NoteWritingStarted);
        Status.Append("Finalising", busy: true);
        var response = await RequestValueAsync("session/stop", StopTimeout).ConfigureAwait(true);
        if (response is null)
        {
            // A failed stop must not wedge the UI; the recording is safe in
            // the store either way
            State = SessionState.Idle;
            Note.Reset();
            Status.Append("Stop failed - session kept");
            return;
        }

        // The finalised transcript carries the speaker labels the live feed
        // could not; it replaces the pane once the engine has sealed it
        if (response is { ValueKind: JsonValueKind.Object } stop
            && stop.TryGetProperty("sessionId", out var sessionId))
        {
            _finalisedSessionId = sessionId.GetString();
            await LoadFinalTranscriptAsync(_finalisedSessionId).ConfigureAwait(true);
        }
    }

    private async Task LoadFinalTranscriptAsync(string? id)
    {
        if (string.IsNullOrEmpty(id))
        {
            Phase = FinalisePhase.Note;  // nothing to fetch; the panes still open
            return;
        }

        try
        {
            var response = await _engine
                .RequestAsync("session/transcript", new { id }, RequestTimeout)
                .ConfigureAwait(true);
            Transcript.Turns.Clear();
            foreach (var turn in response.GetProperty("turns").EnumerateArray())
            {
                Transcript.Add(
                    turn.GetProperty("speaker").GetString() ?? "",
                    turn.GetProperty("firstFrame").GetUInt64(),
                    turn.GetProperty("text").GetString() ?? "");
            }
        }
        catch (Exception)
        {
            Status.Append("Could not load transcript");
        }
        finally
        {
            Phase = FinalisePhase.Note;
        }
    }

    public async Task CancelRecordingAsync()
    {
        if (State != SessionState.Recording)
        {
            return;
        }

        if (!await RequestAsync("session/cancel").ConfigureAwait(true))
        {
            return;
        }

        State = SessionState.Idle;
        Paused = false;
        ActiveReplay = null;
        Status.SetMicVisible(false);
        Status.Append("Cancelled");
    }

    public void StartNewConsultation()
    {
        if (State != SessionState.Review)
        {
            return;
        }

        Note.Reset();
        Transcript.Clear();
        Phase = FinalisePhase.None;
        _regenerating = false;
        State = SessionState.Idle;
        Status.Append("Ready");
    }

    private void HandleNotification(string method, JsonElement parameters)
    {
        switch (method)
        {
            // Finalise stages advance the phase; the status bar keeps its one
            // "Finalising". Stages the engine skips never show, and a stage
            // arriving after the seal cannot move the phase backwards
            case "session/progress" when State == SessionState.Finalising
                && Phase < FinalisePhase.Note
                && parameters.ValueKind == JsonValueKind.Object:
                Phase = parameters.GetProperty("stage").GetString() switch
                {
                    "transcript" => FinalisePhase.Transcript,
                    "speakers" => FinalisePhase.Speakers,
                    _ => Phase,
                };
                break;
            // The note pane shows the note being written, then the sealed
            // text. The status states writing only once tokens actually
            // stream - a thin recording writes nothing and must never claim
            // to. Review is included because a regenerate streams there.
            case "note/partial" when State is SessionState.Finalising or SessionState.Review
                && parameters.ValueKind == JsonValueKind.Object:
                if (Note.ClinicalNoteText.Length == 0)
                {
                    Status.Append("Writing clinical note", busy: true);
                    Phase = FinalisePhase.Streaming;  // the panes open on the first token
                }

                Note.ClinicalNoteText = parameters.GetProperty("text").GetString() ?? "";
                if (!_regenerating)
                {
                    _metrics?.NotePartial();
                }

                break;
            case "note/ready" when State is SessionState.Finalising or SessionState.Review:
                if (parameters.ValueKind == JsonValueKind.Object
                    && parameters.TryGetProperty("text", out var noteText))
                {
                    Note.ClinicalNoteText = noteText.GetString() ?? "";
                }

                Note.Apply(NotePipelineEvent.NoteReady);
                State = SessionState.Review;
                if (_metrics is not null && !_regenerating)
                {
                    _ = _metrics.SessionFinishedAsync(null, Note.ClinicalNoteText.Length);
                }

                break;
            // The transcript is still usable, so review proceeds without a note
            case "note/failed" when State is SessionState.Finalising or SessionState.Review:
                Note.Apply(NotePipelineEvent.NoteFailed);
                State = SessionState.Review;
                var noteFailure = parameters.ValueKind == JsonValueKind.Object
                    ? parameters.GetProperty("detail").GetString() ?? "failed"
                    : "failed";
                Status.Append("Clinical note failed");
                if (_metrics is not null && !_regenerating)
                {
                    _ = _metrics.SessionFinishedAsync(noteFailure, 0);
                }

                _regenerating = false;
                break;
            case "patient/partial" when parameters.ValueKind == JsonValueKind.Object:
                if (Note.PatientInfoText.Length == 0)
                {
                    Status.Append("Writing patient note", busy: true);
                }

                Note.PatientInfoText = parameters.GetProperty("text").GetString() ?? "";
                break;
            case "patient/ready":
                if (parameters.ValueKind == JsonValueKind.Object
                    && parameters.TryGetProperty("text", out var patientText))
                {
                    Note.PatientInfoText = patientText.GetString() ?? "";
                }

                Note.Apply(NotePipelineEvent.PatientInfoReady);
                Status.Append("Ready for review");
                _regenerating = false;
                break;
            case "patient/failed":
                Note.Apply(NotePipelineEvent.PatientInfoFailed);
                Status.Append("Patient note failed");
                _regenerating = false;
                break;
            case "translate/partial" when parameters.ValueKind == JsonValueKind.Object:
                Note.TranslationText = parameters.GetProperty("text").GetString() ?? "";
                break;
            case "translate/ready" when parameters.ValueKind == JsonValueKind.Object:
                Note.TranslationText = parameters.GetProperty("text").GetString() ?? "";
                Note.TranslationRunning = false;
                Status.Append($"Translated to {parameters.GetProperty("language").GetString()}");
                break;
            case "translate/failed":
                Note.TranslationRunning = false;
                Status.Append("Translation failed");
                break;
            case "audio.level" when parameters.ValueKind == JsonValueKind.Object:
                Status.SetMicLevel(
                    parameters.GetProperty("level").GetDouble(),
                    parameters.GetProperty("clipped").GetBoolean());
                if (State == SessionState.Recording)
                {
                    AudioSeconds += 0.1;
                }

                break;
            case "session/interrupted"
                when State is SessionState.Recording or SessionState.Finalising:
                State = SessionState.Idle;
                Paused = false;
                ActiveReplay = null;
                Note.Reset();
                Status.SetMicVisible(false);
                Status.Append(parameters.ValueKind == JsonValueKind.Object
                    ? $"Recording interrupted ({parameters.GetProperty("detail").GetString()}) - session kept"
                    : "Recording interrupted - session kept");
                break;
            default:
                break;
        }
    }

    private async Task<bool> RequestAsync(
        string method, object? parameters = null, TimeSpan? timeout = null) =>
        await RequestValueAsync(method, timeout, parameters).ConfigureAwait(true) is not null;

    private async Task<JsonElement?> RequestValueAsync(
        string method, TimeSpan? timeout = null, object? parameters = null)
    {
        try
        {
            return await _engine.RequestAsync(method, parameters, timeout ?? RequestTimeout)
                .ConfigureAwait(true);
        }
        catch (OperationCanceledException)
        {
            Status.Append("Taking longer than expected", busy: true);
            return null;
        }
        catch (Exception e)
        {
            Status.Append("A step failed - trying to continue");
            Status.Log($"{method} failed: {e.Message}");
            return null;
        }
    }
}
