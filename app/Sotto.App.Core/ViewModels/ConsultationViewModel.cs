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
            (method, _) => dispatcher.Post(() => HandleNotification(method));
    }

    public async Task StartRecordingAsync()
    {
        if (State != SessionState.Idle)
        {
            return;
        }

        await RequestAsync("session/start").ConfigureAwait(true);
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
        await RequestAsync("session/stop").ConfigureAwait(true);
    }

    public async Task CancelRecordingAsync()
    {
        if (State != SessionState.Recording)
        {
            return;
        }

        await RequestAsync("session/cancel").ConfigureAwait(true);
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

    private void HandleNotification(string method)
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
            default:
                break;
        }
    }

    private async Task RequestAsync(string method)
    {
        try
        {
            _ = await _engine.RequestAsync(method, null, RequestTimeout).ConfigureAwait(true);
        }
        catch (Exception e) when (e is not OperationCanceledException)
        {
            Status.Append($"engine request {method} failed: {e.Message}");
        }
    }
}
