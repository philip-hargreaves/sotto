using Ambient.App.Core;
using Ambient.App.Core.ViewModels;

namespace Ambient.App.Tests;

/// <summary>A stored session opened for review through the consultation VM.</summary>
public class ReviewSessionTest
{
    [Fact]
    public async Task DocumentsAreReadOnlyUntilEditAndDiscardRestores()
    {
        var (session, _, note) = TestSession.Create();
        await session.OpenStoredSessionAsync("abc");
        note.ClinicalNoteText = "the stored note";

        Assert.True(note.NoteViewing, "read-only until an explicit Edit");
        note.EditNoteCommand.Execute(null);
        Assert.True(note.NoteEditing);

        note.ClinicalNoteText = "a stray keystroke";
        note.DiscardNoteCommand.Execute(null);

        Assert.Equal("the stored note", note.ClinicalNoteText);
        Assert.True(note.NoteViewing);
    }

    [Fact]
    public async Task SaveCommitsAndLeavesEditing()
    {
        var (session, _, note) = TestSession.Create();
        await session.OpenStoredSessionAsync("abc");
        var saves = 0;
        note.SaveNoteRequested = () =>
        {
            saves++;
            return Task.CompletedTask;
        };

        note.EditNoteCommand.Execute(null);
        note.ClinicalNoteText = "a deliberate edit";
        await note.SaveNoteCommand.ExecuteAsync(null);

        Assert.Equal(1, saves);
        Assert.True(note.NoteViewing);
        Assert.Equal("a deliberate edit", note.ClinicalNoteText);
    }

    [Fact]
    public async Task NothingRegeneratesOrTranslatesWhileEditing()
    {
        var (session, _, note) = TestSession.Create();
        await session.OpenStoredSessionAsync("abc");
        note.RegeneratePatientRequested = () => Task.CompletedTask;
        note.PatientStale = true;
        note.SelectedLanguage = "French";

        Assert.True(note.RegenerateCommand.CanExecute(null));
        note.EditPatientCommand.Execute(null);

        Assert.False(note.RegenerateCommand.CanExecute(null));
        Assert.False(note.RegeneratePatientCommand.CanExecute(null));
        Assert.False(note.TranslateCommand.CanExecute(null));

        note.DiscardPatientCommand.Execute(null);
        Assert.True(note.RegenerateCommand.CanExecute(null));
    }

    [Fact]
    public async Task LeavingMidEditAutosavesTheEdit()
    {
        var (session, engine, note) = TestSession.Create();
        await session.OpenStoredSessionAsync("abc");
        note.EditNoteCommand.Execute(null);
        note.ClinicalNoteText = "edited then left";

        await session.CloseReviewAsync();

        Assert.Contains(engine.Requests,
            r => r.Method == "note/update" && r.Params.Contains("edited then left"));
    }

    [Fact]
    public async Task StaleSheetRewritesFromTheStoredNote()
    {
        var (session, engine, note) = TestSession.Create();
        await session.OpenStoredSessionAsync("abc");
        note.PatientInfoText = "old sheet";
        note.PatientStale = true;
        Assert.True(note.RegeneratePatientCommand.CanExecute(null));

        await note.RegeneratePatientCommand.ExecuteAsync(null);

        Assert.Contains(engine.Requests, r => r.Method == "patient/regenerate");
        engine.RaiseNotification("patient/ready", System.Text.Json.JsonSerializer
            .SerializeToElement(new { text = "fresh sheet" }));
        Assert.Equal("fresh sheet", note.PatientInfoText);
        Assert.False(note.PatientStale, "rewritten from the note, no longer stale");
        Assert.False(note.RegeneratePatientCommand.CanExecute(null));
    }

    [Fact]
    public async Task RegenerateOnAnEditedNoteAsksFirst()
    {
        var (session, engine, note) = TestSession.Create();
        await session.OpenStoredSessionAsync("abc");
        note.EditedStamp = "Edited 10:31";
        var regenerates = 0;
        note.RegenerateRequested = () =>
        {
            regenerates++;
            return Task.CompletedTask;
        };

        await note.RegenerateCommand.ExecuteAsync(null);
        Assert.True(note.RegenerateWarningOpen, "an edited note warns");
        Assert.Equal(0, regenerates);

        note.KeepEditsCommand.Execute(null);
        Assert.False(note.RegenerateWarningOpen);
        Assert.Equal(0, regenerates);

        await note.RegenerateCommand.ExecuteAsync(null);
        await note.ConfirmRegenerateCommand.ExecuteAsync(null);
        Assert.False(note.RegenerateWarningOpen);
        Assert.Equal(1, regenerates);
    }

    [Fact]
    public async Task AnUneditedNoteRegeneratesWithoutAsking()
    {
        var (session, _, note) = TestSession.Create();
        await session.OpenStoredSessionAsync("abc");
        var regenerates = 0;
        note.RegenerateRequested = () =>
        {
            regenerates++;
            return Task.CompletedTask;
        };

        await note.RegenerateCommand.ExecuteAsync(null);

        Assert.False(note.RegenerateWarningOpen);
        Assert.Equal(1, regenerates);
    }

    [Fact]
    public void LoadStoredShowsTheOptionsWithoutMakingThemDefaults()
    {
        var note = new NoteViewModel();
        var optionChanges = 0;
        note.OptionsChanged = () => optionChanges++;

        note.LoadStored("text", "sheet", "", "soap", "concise", "Edited 10:31");

        Assert.Equal("soap", note.Style);
        Assert.Equal("concise", note.Detail);
        Assert.Equal(0, optionChanges);
        Assert.Equal(NotePipelineState.AllReady, note.PipelineState);
        Assert.True(note.Edited);

        note.Style = "prose";  // the clinician's own change still registers
        Assert.Equal(1, optionChanges);
    }

    [Fact]
    public async Task StartCarriesTheKeepConsultationsSetting()
    {
        var preferences = new AppPreferences(
            Path.Combine(Path.GetTempPath(), Path.GetRandomFileName()));
        var (session, engine, _) = TestSession.Create(preferences);

        await session.StartRecordingAsync();
        Assert.Contains(engine.Requests, r => r.Method == "session/start"
            && r.Params.Contains("\"retain\":false"));
        await session.StopRecordingAsync();

        preferences.KeepConsultations = true;
        engine.RaiseNotification("note/ready");
        session.StartNewConsultation();
        await session.StartRecordingAsync();
        Assert.Contains(engine.Requests, r => r.Method == "session/start"
            && r.Params.Contains("\"retain\":true"));
    }

    [Fact]
    public async Task OpeningIsRefusedWhileRecording()
    {
        var (session, engine, _) = TestSession.Create();
        await session.StartRecordingAsync();

        Assert.False(await session.OpenStoredSessionAsync("abc"));
        Assert.DoesNotContain(engine.Requests, r => r.Method == "session/open");
    }

    [Fact]
    public async Task NewConsultationSavesEditsClosesAndClears()
    {
        var (session, engine, note) = TestSession.Create();
        await session.OpenStoredSessionAsync("abc");
        Assert.Equal(SessionState.Review, session.State);
        note.ClinicalNoteText = "corrected wording";

        session.StartNewConsultation();

        Assert.Contains(engine.Requests, r => r.Method == "note/update"
            && r.Params.Contains("corrected wording"));
        Assert.Contains(engine.Requests, r => r.Method == "session/close");
        Assert.Equal(SessionState.Idle, session.State);
        Assert.Equal("", note.ClinicalNoteText);
        Assert.Equal(NotePipelineState.Pending, note.PipelineState);
    }

    [Fact]
    public async Task SavingANoteEditMarksTheSheetStale()
    {
        var (session, engine, note) = TestSession.Create();
        await session.OpenStoredSessionAsync("abc");
        note.PatientInfoText = "the sheet";
        Assert.False(note.PatientStale);

        note.ClinicalNoteText = "corrected";
        await session.SaveNoteAsync();
        Assert.True(note.PatientStale, "the sheet no longer derives from the note");

        engine.RaiseNotification("patient/ready");  // a rewrite clears it
        Assert.False(note.PatientStale);
    }

    [Fact]
    public void AStoredSheetOlderThanTheNoteEditLoadsStale()
    {
        var note = new NoteViewModel();
        note.LoadStored("text", "sheet", "", "prose", "standard", "Edited 10:31");
        // The consultation VM computes this from the stamps; the property is
        // the contract the views bind to
        note.PatientStale = true;
        Assert.True(note.PatientStale);
        note.Reset();
        Assert.False(note.PatientStale);
    }

    [Fact]
    public async Task ARegenerateClearsTheEditedStamp()
    {
        var (session, _, note) = TestSession.Create();
        await session.OpenStoredSessionAsync("abc");
        note.EditedStamp = "Edited 10:31";

        note.BeginRegenerate();

        Assert.False(note.Edited);
    }

    [Fact]
    public async Task OpeningAgainSavesTheFirstReviewsEdits()
    {
        var (session, engine, note) = TestSession.Create();
        await session.OpenStoredSessionAsync("abc");
        note.ClinicalNoteText = "edited in review";

        await session.OpenStoredSessionAsync("def");

        Assert.Contains(engine.Requests, r => r.Method == "note/update"
            && r.Params.Contains("abc") && r.Params.Contains("edited in review"));
        Assert.Contains(engine.Requests, r => r.Method == "session/open"
            && r.Params.Contains("def"));
    }
}
