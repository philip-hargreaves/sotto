using System.Globalization;
using System.Text.Json;
using Ambient.App.Core.Hosting;
using Ambient.Client;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace Ambient.App.Core.ViewModels;

/// <summary>
/// The clinician's voiceprint as one thing: learned from consultations, or set
/// up by reading a passage and refined by consultations after that. Never two.
/// </summary>
public sealed partial class VoiceViewModel : ObservableObject
{
    // Below this the automatic print is still settling; the copy says so
    private const int LearnedAfterSessions = 5;

    private readonly IEngineClient _engine;
    private readonly ISessionState? _session;
    private readonly StatusBarViewModel? _status;
    private readonly TimeProvider _clock;

    public VoiceViewModel(IEngineClient engine, ISessionState? session = null,
        StatusBarViewModel? status = null, TimeProvider? clock = null)
    {
        _engine = engine;
        _session = session;
        _status = status;
        _clock = clock ?? TimeProvider.System;
        _engine.ConnectedChanged += connected =>
        {
            NotifyCommands();
            if (connected)
            {
                _ = RefreshAsync();
            }
        };
    }

    /// <summary>"none", "accrued" or "enrolled", as the engine reports it.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Headline), nameof(HasVoice), nameof(SetUpLabel))]
    [NotifyCanExecuteChangedFor(nameof(ForgetVoiceCommand))]
    public partial string Origin { get; private set; } = "none";

    /// <summary>Consultations that refined the print since it began.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Headline))]
    public partial int Sessions { get; private set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Headline))]
    public partial DateTimeOffset? EnrolledAt { get; private set; }

    /// <summary>An enrolment or a clear in flight.</summary>
    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(SetUpVoiceCommand), nameof(ForgetVoiceCommand))]
    public partial bool Busy { get; private set; }

    /// <summary>The view runs the enrolment dialog; true when a print was made.</summary>
    public Func<Task<bool>>? RunEnrolment { get; set; }

    /// <summary>Forgetting is irreversible, so the view confirms it once.</summary>
    public Func<Task<bool>>? ConfirmForget { get; set; }

    public bool HasVoice => Origin != "none";

    public string SetUpLabel => HasVoice ? "Redo" : "Set up";

    public string Headline => Origin switch
    {
        "enrolled" => EnrolledDescription(),
        "accrued" when Sessions < LearnedAfterSessions =>
            $"Learning automatically, {Plural(Sessions, "consultation")} so far",
        "accrued" => $"Learned automatically from {Plural(Sessions, "consultation")}",
        _ => "Tells you apart from the patient. Learned automatically, or set up now.",
    };

    public async Task RefreshAsync()
    {
        if (!_engine.Connected)
        {
            return;
        }

        try
        {
            var status = await _engine
                .RequestAsync("anchor/status", null, TimeSpan.FromSeconds(5))
                .ConfigureAwait(true);
            Origin = status.GetProperty("origin").GetString() ?? "none";
            Sessions = status.GetProperty("sessions").GetInt32();
            EnrolledAt = status.TryGetProperty("enrolledAt", out var at)
                         && at.ValueKind == JsonValueKind.Number
                ? DateTimeOffset.FromUnixTimeSeconds(at.GetInt64())
                : null;
        }
        catch (Exception)
        {
            // An unreachable engine leaves the last known state on screen
        }
    }

    [RelayCommand(CanExecute = nameof(CanSetUp))]
    private async Task SetUpVoice()
    {
        if (_session?.ConsultationActive == true)
        {
            _status?.Append("finish the consultation before voice enrolment");
            return;
        }

        Busy = true;
        try
        {
            var made = await RunEnrolment!().ConfigureAwait(true);
            await RefreshAsync().ConfigureAwait(true);
            if (made)
            {
                _status?.Append("voice enrolment complete");
            }
        }
        finally
        {
            Busy = false;
        }
    }

    private bool CanSetUp() => _engine.Connected && !Busy && RunEnrolment is not null;

    [RelayCommand(CanExecute = nameof(CanForget))]
    private async Task ForgetVoice()
    {
        if (_session?.ConsultationActive == true)
        {
            _status?.Append("finish the consultation before forgetting voice enrolment");
            return;
        }

        if (ConfirmForget is not null && !await ConfirmForget().ConfigureAwait(true))
        {
            return;
        }

        Busy = true;
        try
        {
            await _engine.RequestAsync("anchor/clear", null, TimeSpan.FromSeconds(5))
                .ConfigureAwait(true);
            await RefreshAsync().ConfigureAwait(true);
            _status?.Append("voice enrolment forgotten");
        }
        catch (Exception e)
        {
            _status?.Append($"could not forget voice enrolment: {e.Message}");
        }
        finally
        {
            Busy = false;
        }
    }

    private bool CanForget() => _engine.Connected && !Busy && HasVoice;

    /// <summary>Re-evaluate the commands after a connection change.</summary>
    public void NotifyCommands()
    {
        SetUpVoiceCommand.NotifyCanExecuteChanged();
        ForgetVoiceCommand.NotifyCanExecuteChanged();
    }

    private string EnrolledDescription()
    {
        var when = EnrolledAt?.ToLocalTime();
        var date = when is null
            ? "Set up"
            : when.Value.Year == _clock.GetLocalNow().Year
                ? $"Set up on {when.Value.ToString("d MMM", CultureInfo.CurrentCulture)}"
                : $"Set up on {when.Value.ToString("d MMM yyyy", CultureInfo.CurrentCulture)}";
        return Sessions > 0
            ? $"{date}, refined automatically by {Plural(Sessions, "consultation")} since"
            : date;
    }

    private static string Plural(int count, string noun) =>
        count == 1 ? $"1 {noun}" : $"{count} {noun}s";
}
