using System.Collections.ObjectModel;
using System.Globalization;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Sotto.Client;

namespace Sotto.App.Core.ViewModels;

public sealed record SessionRow(
    string Id, string Title, string Started, string Duration, string EditedLabel)
{
    public bool Edited => EditedLabel.Length > 0;
}

/// <summary>
/// Past consultations. Selecting one opens it for review through the
/// consultation view model, so the shared panes show it and regenerate,
/// translate and save act on it.
/// </summary>
public sealed partial class SessionsViewModel : ObservableObject
{
    private static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(5);

    private readonly IEngineClient _engine;
    private readonly StatusBarViewModel _status;
    private readonly ConsultationViewModel _consultation;

    public ObservableCollection<SessionRow> Sessions { get; } = [];

    [ObservableProperty]
    public partial SessionRow? Selected { get; set; }

    /// <summary>True while the selected session is open in the panes.</summary>
    [ObservableProperty]
    public partial bool DetailOpen { get; private set; }

    /// <summary>The open session's label; editing it renames the session.</summary>
    [ObservableProperty]
    public partial string DetailTitle { get; set; } = "";

    [ObservableProperty]
    public partial string DetailMeta { get; private set; } = "";

    public NoteViewModel Note => _consultation.Note;

    public SessionsViewModel(
        IEngineClient engine, StatusBarViewModel status, ConsultationViewModel consultation)
    {
        _engine = engine;
        _status = status;
        _consultation = consultation;
    }

    // True while a rename swaps the selected row for its retitled copy;
    // that reselection must not reopen the session
    private bool _retitling;

    partial void OnSelectedChanged(SessionRow? value)
    {
        if (!_retitling)
        {
            _ = OpenAsync(value);
        }
    }

    [RelayCommand]
    public async Task RefreshAsync()
    {
        try
        {
            var result = await _engine
                .RequestAsync("session/list", null, RequestTimeout).ConfigureAwait(true);
            Selected = null;
            Sessions.Clear();
            foreach (var session in result.GetProperty("sessions").EnumerateArray())
            {
                var started = session.GetProperty("startedAt").GetString() ?? "";
                var ended = session.GetProperty("endedAt").GetString() ?? "";
                var label = session.TryGetProperty("label", out var l)
                    ? l.GetString() ?? "" : "";
                var edited = session.TryGetProperty("editedAt", out var e)
                    && e.ValueKind == System.Text.Json.JsonValueKind.String
                    ? e.GetString() ?? "" : "";
                // No title beats a bad title: without a stored label the
                // date and time are the row's name
                var startedLabel = FormatStarted(started);
                var audioSeconds = session.TryGetProperty("audioSeconds", out var a)
                    ? a.GetDouble() : 0;
                Sessions.Add(new SessionRow(
                    session.GetProperty("id").GetString() ?? "",
                    label.Length > 0 ? label : startedLabel,
                    startedLabel,
                    FormatDuration(audioSeconds, started, ended),
                    EditedStamp.Label(started, edited)));
            }
        }
        catch (Exception e) when (e is not OperationCanceledException)
        {
            _status.Append($"could not list sessions: {e.Message}");
        }
    }

    private async Task OpenAsync(SessionRow? row)
    {
        if (row is null)
        {
            DetailOpen = false;
            return;
        }

        DetailOpen = await _consultation.OpenStoredSessionAsync(row.Id, row.Started)
            .ConfigureAwait(true);
        if (DetailOpen)
        {
            DetailTitle = row.Title;
            DetailMeta = $"{row.Started} · {row.Duration} · {OptionsLabel()}";
        }
    }

    /// <summary>Commits an edited title as the session's label.</summary>
    [RelayCommand]
    public async Task RenameAsync()
    {
        if (Selected is null || DetailTitle.Length == 0 || DetailTitle == Selected.Title)
        {
            return;
        }

        try
        {
            _ = await _engine
                .RequestAsync("session/label", new { id = Selected.Id, text = DetailTitle },
                    RequestTimeout)
                .ConfigureAwait(true);
            var index = Sessions.IndexOf(Selected);
            var renamed = Selected with { Title = DetailTitle };
            _retitling = true;
            try
            {
                Sessions[index] = renamed;  // replacing the item deselects it
                Selected = renamed;
            }
            finally
            {
                _retitling = false;
            }
        }
        catch (Exception e) when (e is not OperationCanceledException)
        {
            _status.Append($"could not rename: {e.Message}");
        }
    }

    [RelayCommand]
    public async Task DeleteSelectedAsync()
    {
        if (Selected is null)
        {
            return;
        }

        try
        {
            await _consultation.CloseReviewAsync().ConfigureAwait(true);
            _ = await _engine
                .RequestAsync("session/delete", new { id = Selected.Id }, RequestTimeout)
                .ConfigureAwait(true);
            _status.Append("Session deleted");
            DetailOpen = false;
            await RefreshAsync().ConfigureAwait(true);
        }
        catch (Exception e) when (e is not OperationCanceledException)
        {
            _status.Append($"could not delete session: {e.Message}");
        }
    }

    /// <summary>Leaving the page ends the review and saves any edits.</summary>
    public Task LeaveAsync()
    {
        Selected = null;
        DetailOpen = false;
        return _consultation.CloseReviewAsync();
    }

    private string OptionsLabel()
    {
        var style = Note.Style switch { "soap" => "SOAP", _ => "Prose" };
        var detail = Note.Detail switch
        {
            "concise" => "concise",
            "detailed" => "detailed",
            _ => "standard",
        };
        return $"{style}, {detail}";
    }

    private static string FormatStarted(string startedAt) =>
        DateTimeOffset.TryParse(startedAt, CultureInfo.InvariantCulture, out var started)
            ? started.ToLocalTime().ToString("d MMM HH:mm", CultureInfo.CurrentCulture)
            : startedAt;

    // The consultation's length is its audio, which a fast replay records in
    // seconds of wall time; the wall clock is only the fallback
    private static string FormatDuration(double audioSeconds, string startedAt, string endedAt)
    {
        var seconds = audioSeconds;
        if (seconds <= 0)
        {
            if (!DateTimeOffset.TryParse(startedAt, CultureInfo.InvariantCulture, out var started)
                || !DateTimeOffset.TryParse(endedAt, CultureInfo.InvariantCulture, out var ended))
            {
                return "";
            }

            seconds = (ended - started).TotalSeconds;
        }

        // A duration, unmistakably not a second clock time
        return seconds < 90 ? "1 min" : $"{(int)Math.Round(seconds / 60)} min";
    }
}
