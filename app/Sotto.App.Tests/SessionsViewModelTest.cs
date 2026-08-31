using System.Text.Json;
using Sotto.App.Core.ViewModels;

namespace Sotto.App.Tests;

public class SessionsViewModelTest
{
    private static (SessionsViewModel Sessions, ConsultationViewModel Consultation,
        RecordingEngineClient Engine, StatusBarViewModel Status) Create()
    {
        var engine = new RecordingEngineClient();
        var note = new NoteViewModel();
        var status = new StatusBarViewModel();
        var consultation = new ConsultationViewModel(
            engine, new InlineDispatcher(), new TranscriptViewModel(), note, status);
        return (new SessionsViewModel(engine, status, consultation), consultation, engine, status);
    }

    /// <summary>Scripted responses per method; unscripted methods answer {}.</summary>
    public sealed class RecordingEngineClient : Sotto.Client.IEngineClient
    {
        public Dictionary<string, object> Responses { get; } = [];

        public List<(string Method, string Params)> Calls { get; } = [];

        public HashSet<string> Failing { get; } = [];

        public event Action<string, JsonElement>? NotificationReceived
        {
            add { }
            remove { }
        }

        public event Action<bool>? ConnectedChanged
        {
            add { }
            remove { }
        }

        public bool Connected => true;

        public Task<JsonElement> RequestAsync(
            string method, object? parameters, TimeSpan timeout,
            CancellationToken cancellationToken = default)
        {
            Calls.Add((method, parameters is null ? "" : JsonSerializer.Serialize(parameters)));
            if (Failing.Contains(method))
            {
                throw new InvalidOperationException($"{method} refused");
            }

            return Task.FromResult(JsonSerializer.SerializeToElement(
                Responses.TryGetValue(method, out var r) ? r : new { }));
        }

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }

    private static void ScriptOneSession(RecordingEngineClient engine)
    {
        engine.Responses["session/list"] = new
        {
            sessions = new[]
            {
                new
                {
                    id = "abc",
                    startedAt = "2026-08-17T10:15:00Z",
                    endedAt = "2026-08-17T10:23:41Z",
                    state = "finalised",
                    sampleRate = 16000,
                    label = "Elbow swelling",
                    editedAt = "2026-08-17T10:31:00Z",
                    audioSeconds = 542.0,  // a 16x replay: wall clock says 8:41, the audio 9 min
                },
            },
        };
        engine.Responses["session/transcript"] = new
        {
            turns = new[]
            {
                new { firstFrame = 480000L, frameCount = 48000L, speaker = "doctor", text = "hello" },
            },
        };
        engine.Responses["session/note"] = new
        {
            text = "the note",
            style = "soap",
            detail = "concise",
            generatedAt = "2026-08-17T10:24:00Z",
            editedAt = "2026-08-17T10:31:00Z",
        };
        engine.Responses["session/patient"] = new
        {
            text = "the sheet",
            language = "en",
            editedAt = (string?)null,
            translation = new { language = "pl", text = "arkusz" },
        };
    }

    [Fact]
    public async Task RefreshListsSessionsWithLabelAndEditStamp()
    {
        var (vm, _, engine, _) = Create();
        ScriptOneSession(engine);

        await vm.RefreshAsync();

        var row = Assert.Single(vm.Sessions);
        Assert.Equal("abc", row.Id);
        Assert.Equal("Elbow swelling", row.Title);
        Assert.Equal("9 min", row.Duration);
        Assert.True(row.Edited);
        Assert.StartsWith("Edited ", row.EditedLabel);
    }

    [Fact]
    public async Task AMissingLabelFallsBackToTheDateAndTime()
    {
        var (vm, _, engine, _) = Create();
        engine.Responses["session/list"] = new
        {
            sessions = new[]
            {
                new
                {
                    id = "abc",
                    startedAt = "2026-08-17T10:15:00Z",
                    endedAt = "2026-08-17T10:23:41Z",
                    state = "finalised",
                    sampleRate = 16000,
                    label = "",
                    editedAt = (string?)null,
                },
            },
        };

        await vm.RefreshAsync();

        var row = Assert.Single(vm.Sessions);
        Assert.Equal(row.Started, row.Title);
        Assert.False(row.Edited);
    }

    [Fact]
    public async Task SelectingOpensTheSessionIntoTheSharedPanes()
    {
        var (vm, consultation, engine, _) = Create();
        ScriptOneSession(engine);
        await vm.RefreshAsync();

        vm.Selected = vm.Sessions[0];
        await Task.Delay(50);

        Assert.True(vm.DetailOpen);
        Assert.Contains(engine.Calls, c => c.Method == "session/open" && c.Params.Contains("abc"));
        Assert.Equal(SessionState.Review, consultation.State);
        Assert.Equal("the note", consultation.Note.ClinicalNoteText);
        Assert.Equal("the sheet", consultation.Note.PatientInfoText);
        Assert.Equal("arkusz", consultation.Note.TranslationText);
        Assert.Equal("pl", consultation.Note.TranslationLanguage);
        Assert.Equal("pl translation", consultation.Note.TranslationCaption);
        Assert.Equal("soap", consultation.Note.Style);
        Assert.True(consultation.Note.Edited);
        Assert.Single(consultation.Transcript.Turns);
        Assert.Equal("Elbow swelling", vm.DetailTitle);
        Assert.Contains("SOAP, concise", vm.DetailMeta);
    }

    [Fact]
    public async Task AnOpenTheEngineRefusesLeavesTheDetailClosed()
    {
        var (vm, consultation, engine, _) = Create();
        ScriptOneSession(engine);
        engine.Failing.Add("session/open");
        await vm.RefreshAsync();

        vm.Selected = vm.Sessions[0];
        await Task.Delay(50);

        Assert.False(vm.DetailOpen);
        Assert.Equal(SessionState.Idle, consultation.State);
    }

    [Fact]
    public async Task RenamingSendsTheLabelAndUpdatesTheRow()
    {
        var (vm, _, engine, _) = Create();
        ScriptOneSession(engine);
        await vm.RefreshAsync();
        vm.Selected = vm.Sessions[0];
        await Task.Delay(50);

        vm.DetailTitle = "Left elbow bursitis";
        await vm.RenameAsync();

        Assert.Contains(engine.Calls, c => c.Method == "session/label"
            && c.Params.Contains("Left elbow bursitis"));
        Assert.Equal("Left elbow bursitis", vm.Sessions[0].Title);
        Assert.Same(vm.Sessions[0], vm.Selected);
        Assert.True(vm.DetailOpen, "renaming must not close the open session");
        Assert.Equal(1, engine.Calls.Count(c => c.Method == "session/open"));
    }

    [Fact]
    public async Task LeavingClosesTheReviewAndSavesEdits()
    {
        var (vm, consultation, engine, _) = Create();
        ScriptOneSession(engine);
        await vm.RefreshAsync();
        vm.Selected = vm.Sessions[0];
        await Task.Delay(50);

        consultation.Note.ClinicalNoteText = "the note, corrected";
        await vm.LeaveAsync();

        Assert.Contains(engine.Calls, c => c.Method == "note/update"
            && c.Params.Contains("the note, corrected"));
        Assert.Contains(engine.Calls, c => c.Method == "session/close");
        Assert.Equal(SessionState.Idle, consultation.State);
        Assert.False(vm.DetailOpen);
    }

    [Fact]
    public async Task DeleteClosesTheReviewFirstAndRefreshes()
    {
        var (vm, _, engine, _) = Create();
        ScriptOneSession(engine);
        await vm.RefreshAsync();
        vm.Selected = vm.Sessions[0];
        await Task.Delay(50);

        await vm.DeleteSelectedAsync();

        var close = engine.Calls.FindIndex(c => c.Method == "session/close");
        var delete = engine.Calls.FindIndex(c => c.Method == "session/delete");
        Assert.True(close >= 0 && delete > close, "close precedes delete");
        Assert.Equal(2, engine.Calls.Count(c => c.Method == "session/list"));
    }

    [Fact]
    public async Task EngineErrorsLandInTheStatusLogNotAsCrashes()
    {
        var (vm, _, engine, status) = Create();
        engine.Failing.Add("session/list");

        await vm.RefreshAsync();

        Assert.Empty(vm.Sessions);
        Assert.Contains(status.LogEntries, line => line.Contains("could not list sessions"));
    }

    [Fact]
    public async Task AnEmptyStoreIsAnEmptyListNotAnError()
    {
        var (vm, _, engine, _) = Create();
        engine.Responses["session/list"] = new { sessions = Array.Empty<object>() };

        await vm.RefreshAsync();

        Assert.Empty(vm.Sessions);
        Assert.False(vm.EmptyBecauseOff, "no preference known: a plain empty list");
    }

    [Fact]
    public async Task AnEmptyListExplainsItselfWhenRetentionIsOff()
    {
        var preferences = new Sotto.App.Core.AppPreferences(
            Path.Combine(Path.GetTempPath(), Path.GetRandomFileName()));
        var engine = new RecordingEngineClient();
        var status = new StatusBarViewModel();
        var consultation = new ConsultationViewModel(
            engine, new InlineDispatcher(), new TranscriptViewModel(), new NoteViewModel(),
            status);
        var vm = new SessionsViewModel(engine, status, consultation, preferences);
        engine.Responses["session/list"] = new { sessions = Array.Empty<object>() };

        await vm.RefreshAsync();
        Assert.True(vm.EmptyBecauseOff, "keep is off by default and nothing is stored");

        ScriptOneSession(engine);  // history recorded while keep was on still shows
        await vm.RefreshAsync();
        Assert.False(vm.EmptyBecauseOff);
        Assert.Single(vm.Sessions);
    }
}
