using System.Text.Json;
using CommunityToolkit.Mvvm.ComponentModel;
using Sotto.App.Core.Hosting;
using Sotto.Client;

namespace Sotto.App.Core.ViewModels;

/// <summary>
/// Owns the session lifecycle.
/// </summary>
public sealed partial class ConsultationViewModel : ObservableObject, ISessionState
{
    private static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(5);

    // Stop waits for the tail window's decode, seconds of GPU work
    private static readonly TimeSpan StopTimeout = TimeSpan.FromSeconds(60);

    private readonly IEngineClient _engine;

    [ObservableProperty]
    public partial SessionState State { get; private set; } = SessionState.Idle;

    public bool ConsultationActive => State != SessionState.Idle;

    public TranscriptViewModel Transcript { get; }

    public NoteViewModel Note { get; }

    public StatusBarViewModel Status { get; }

    public ConsultationViewModel(
        IEngineClient engine, IUiDispatcher dispatcher,
        TranscriptViewModel transcript, NoteViewModel note, StatusBarViewModel status)
    {
        _engine = engine;
        Transcript = transcript;
        Note = note;
        Status = status;
        _engine.NotificationReceived +=
            (method, parameters) => dispatcher.Post(() => HandleNotification(method, parameters));
    }

    public async Task StartRecordingAsync()
    {
        if (State != SessionState.Idle)
        {
            return;
        }

        if (!await RequestAsync("session/start").ConfigureAwait(true))
        {
            return;
        }

        State = SessionState.Recording;
        Status.Append("recording started");
    }

    public async Task StopRecordingAsync()
    {
        if (State != SessionState.Recording)
        {
            return;
        }

        State = SessionState.Finalising;
        Note.Apply(NotePipelineEvent.NoteWritingStarted);
        Status.Append("finalising");
        var response = await RequestValueAsync("session/stop", StopTimeout).ConfigureAwait(true);

        // The finalised transcript carries the speaker labels the live feed
        // could not; it replaces the pane once the engine has sealed it
        if (response is { ValueKind: JsonValueKind.Object } stop
            && stop.TryGetProperty("sessionId", out var sessionId))
        {
            await LoadFinalTranscriptAsync(sessionId.GetString()).ConfigureAwait(true);
        }
    }

    private async Task LoadFinalTranscriptAsync(string? id)
    {
        if (string.IsNullOrEmpty(id))
        {
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
                var speaker = turn.GetProperty("speaker").GetString();
                var text = turn.GetProperty("text").GetString() ?? "";
                Transcript.Turns.Add(
                    string.IsNullOrEmpty(speaker) ? text : $"{speaker}: {text}");
            }
        }
        catch (Exception e)
        {
            Status.Append($"could not load the final transcript: {e.Message}");
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
        Status.Append("recording cancelled");
    }

    public void StartNewConsultation()
    {
        if (State != SessionState.Review)
        {
            return;
        }

        Note.Reset();
        Transcript.Clear();
        State = SessionState.Idle;
        Status.Append("ready for a new consultation");
    }

    private void HandleNotification(string method, JsonElement parameters)
    {
        switch (method)
        {
            case "note/ready" when State == SessionState.Finalising:
                Note.Apply(NotePipelineEvent.NoteReady);
                State = SessionState.Review;
                Status.Append("clinical note ready");
                break;
            case "patient/ready":
                Note.Apply(NotePipelineEvent.PatientInfoReady);
                Status.Append("patient information ready");
                break;
            case "audio.level" when parameters.ValueKind == JsonValueKind.Object:
                Status.SetMicLevel(
                    parameters.GetProperty("level").GetDouble(),
                    parameters.GetProperty("clipped").GetBoolean());
                break;
            case "session/interrupted"
                when State is SessionState.Recording or SessionState.Finalising:
                State = SessionState.Idle;
                Note.Reset();
                Status.SetMicLevel(0, false);
                Status.Append(parameters.ValueKind == JsonValueKind.Object
                    ? $"session interrupted: {parameters.GetProperty("detail").GetString()}"
                    : "session interrupted");
                break;
            default:
                break;
        }
    }

    private async Task<bool> RequestAsync(string method, TimeSpan? timeout = null) =>
        await RequestValueAsync(method, timeout).ConfigureAwait(true) is not null;

    private async Task<JsonElement?> RequestValueAsync(string method, TimeSpan? timeout = null)
    {
        try
        {
            return await _engine.RequestAsync(method, null, timeout ?? RequestTimeout)
                .ConfigureAwait(true);
        }
        catch (OperationCanceledException)
        {
            Status.Append($"engine request {method} timed out");
            return null;
        }
        catch (Exception e)
        {
            Status.Append($"engine request {method} failed: {e.Message}");
            return null;
        }
    }
}
