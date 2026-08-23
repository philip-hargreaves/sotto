using System.Collections.ObjectModel;
using System.Globalization;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Sotto.Client;

namespace Sotto.App.Core.ViewModels;

public sealed record SessionRow(string Id, string Started, string Duration, string State);

public sealed record TurnRow(string Timestamp, string Speaker, string Text);

/// <summary>
/// Past sessions: list them, read a transcript, delete one.
/// </summary>
public sealed partial class SessionsViewModel : ObservableObject
{
    private static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(5);

    private readonly IEngineClient _engine;
    private readonly StatusBarViewModel _status;

    public ObservableCollection<SessionRow> Sessions { get; } = [];

    public ObservableCollection<TurnRow> Turns { get; } = [];

    [ObservableProperty]
    public partial SessionRow? Selected { get; set; }

    public SessionsViewModel(IEngineClient engine, StatusBarViewModel status)
    {
        _engine = engine;
        _status = status;
    }

    partial void OnSelectedChanged(SessionRow? value) => _ = LoadTranscriptAsync(value);

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
                Sessions.Add(new SessionRow(
                    session.GetProperty("id").GetString() ?? "",
                    FormatStarted(started),
                    FormatDuration(started, ended),
                    session.GetProperty("state").GetString() ?? ""));
            }
        }
        catch (Exception e) when (e is not OperationCanceledException)
        {
            _status.Append($"could not list sessions: {e.Message}");
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
            _ = await _engine
                .RequestAsync("session/delete", new { id = Selected.Id }, RequestTimeout)
                .ConfigureAwait(true);
            _status.Append("Session deleted");
            await RefreshAsync().ConfigureAwait(true);
        }
        catch (Exception e) when (e is not OperationCanceledException)
        {
            _status.Append($"could not delete session: {e.Message}");
        }
    }

    private async Task LoadTranscriptAsync(SessionRow? row)
    {
        Turns.Clear();
        if (row is null)
        {
            return;
        }

        try
        {
            var result = await _engine
                .RequestAsync("session/transcript", new { id = row.Id }, RequestTimeout)
                .ConfigureAwait(true);
            foreach (var turn in result.GetProperty("turns").EnumerateArray())
            {
                Turns.Add(new TurnRow(
                    Clock(turn.GetProperty("firstFrame").GetInt64()),
                    turn.GetProperty("speaker").GetString() ?? "",
                    turn.GetProperty("text").GetString() ?? ""));
            }
        }
        catch (Exception e) when (e is not OperationCanceledException)
        {
            _status.Append($"could not read transcript: {e.Message}");
        }
    }

    private static string Clock(long firstFrame)
    {
        var seconds = firstFrame / 16000;
        return $"{seconds / 60:00}:{seconds % 60:00}";
    }

    private static string FormatStarted(string startedAt) =>
        DateTimeOffset.TryParse(startedAt, CultureInfo.InvariantCulture, out var started)
            ? started.ToLocalTime().ToString("d MMM HH:mm", CultureInfo.CurrentCulture)
            : startedAt;

    private static string FormatDuration(string startedAt, string endedAt)
    {
        if (!DateTimeOffset.TryParse(startedAt, CultureInfo.InvariantCulture, out var started) ||
            !DateTimeOffset.TryParse(endedAt, CultureInfo.InvariantCulture, out var ended))
        {
            return "";
        }

        var span = ended - started;
        return $"{(int)span.TotalMinutes:00}:{span.Seconds:00}";
    }
}
