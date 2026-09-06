using System.Text.Json;
using Ambient.App.Core;
using Ambient.App.Core.ViewModels;

namespace Ambient.App.Tests;

public class NoteOptionsTest
{
    private static JsonElement Params(object value) => JsonSerializer.SerializeToElement(value);

    [Fact]
    public void PersistedOptionsAreAppliedAndPushedAtStartup()
    {
        var preferences = new AppPreferences(Path.Combine(Path.GetTempPath(), "none.json"))
        {
            NoteStyle = "soap",
            NoteDetail = "detailed",
        };

        var (_, engine, note) = TestSession.Create(preferences);

        Assert.Equal("soap", note.Style);
        Assert.Equal("detailed", note.Detail);
        var push = engine.Requests.Single(r => r.Method == "note/options");
        Assert.Contains("soap", push.Params);
        Assert.Contains("detailed", push.Params);
    }

    // The engine never reads a preference: the tier it loads is the one the
    // shell names on connect, before the options and before readiness is asked
    [Fact]
    public void TheNoteTierIsPushedFirstAtStartup()
    {
        var preferences = new AppPreferences(Path.Combine(Path.GetTempPath(), "none.json"))
        {
            NoteTier = "accuracy",
        };

        var (_, engine, _) = TestSession.Create(preferences);

        var methods = engine.Requests.Select(r => r.Method).ToList();
        var tier = engine.Requests.Single(r => r.Method == "note/tier");
        Assert.Contains("accuracy", tier.Params);
        Assert.True(methods.IndexOf("note/tier") < methods.IndexOf("note/options"));
        Assert.True(methods.IndexOf("note/tier") < methods.IndexOf("engine/readiness"));
    }

    [Fact]
    public void AnUnknownStoredTierFallsBackToTheDefault()
    {
        var path = Path.Combine(
            Path.GetTempPath(), $"ambient-test-{Guid.NewGuid():N}", "preferences.json");
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, """{"NoteTier":"premium"}""");

        Assert.Equal("default", AppPreferences.Load(path).NoteTier);

        File.WriteAllText(path, """{"NoteTier":"accuracy"}""");
        Assert.Equal("accuracy", AppPreferences.Load(path).NoteTier);
        Directory.Delete(Path.GetDirectoryName(path)!, recursive: true);
    }

    [Fact]
    public void ChangingAnOptionPersistsItAndInformsTheEngine()
    {
        var path = Path.Combine(
            Path.GetTempPath(), $"ambient-test-{Guid.NewGuid():N}", "preferences.json");
        var (_, engine, note) = TestSession.Create(new AppPreferences(path));

        note.Detail = "concise";

        Assert.Equal("concise", AppPreferences.Load(path).NoteDetail);
        Assert.Contains(engine.Requests,
            r => r.Method == "note/options" && r.Params.Contains("concise"));
        Directory.Delete(Path.GetDirectoryName(path)!, recursive: true);
    }

    [Fact]
    public void UnknownStoredValuesFallBackToDefaults()
    {
        var path = Path.Combine(
            Path.GetTempPath(), $"ambient-test-{Guid.NewGuid():N}", "preferences.json");
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, """{"NoteStyle":"haiku","NoteDetail":"verbose"}""");

        var preferences = AppPreferences.Load(path);

        Assert.Equal("prose", preferences.NoteStyle);
        Assert.Equal("standard", preferences.NoteDetail);
        Directory.Delete(Path.GetDirectoryName(path)!, recursive: true);
    }

    private static async Task<(ConsultationViewModel, FakeEngineClient, NoteViewModel)>
        ReviewedSessionAsync()
    {
        var (session, engine, note) = TestSession.Create();
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        engine.RaiseNotification("note/ready", Params(new { text = "the note" }));
        engine.RaiseNotification("patient/ready", Params(new { text = "the sheet" }));
        return (session, engine, note);
    }

    [Fact]
    public async Task RegenerateRewritesWithTheCurrentOptions()
    {
        var (session, engine, note) = await ReviewedSessionAsync();
        note.Style = "soap";

        Assert.True(note.RegenerateCommand.CanExecute(null));
        await note.RegenerateCommand.ExecuteAsync(null);

        var request = engine.Requests.Single(r => r.Method == "note/regenerate");
        Assert.Contains("soap", request.Params);
        Assert.Equal(NotePipelineState.NoteWriting, note.PipelineState);
        Assert.Equal("", note.ClinicalNoteText);

        // The rewrite streams while the shell already sits in review
        engine.RaiseNotification("note/partial", Params(new { text = "S:" }));
        Assert.Equal("S:", note.ClinicalNoteText);
        engine.RaiseNotification("note/ready", Params(new { text = "S: headache" }));
        engine.RaiseNotification("patient/ready", Params(new { text = "sheet 2" }));

        Assert.Equal(SessionState.Review, session.State);
        Assert.Equal(NotePipelineState.AllReady, note.PipelineState);
        Assert.Equal("S: headache", note.ClinicalNoteText);
    }

    [Fact]
    public async Task RegenerateWaitsForASettledReview()
    {
        var (_, engine, note) = TestSession.Create();
        Assert.False(note.RegenerateCommand.CanExecute(null));

        var (session2, engine2, note2) = await ReviewedSessionAsync();
        await note2.RegenerateCommand.ExecuteAsync(null);
        Assert.False(note2.RegenerateCommand.CanExecute(null));

        engine2.RaiseNotification("note/ready", Params(new { text = "again" }));
        engine2.RaiseNotification("patient/ready", Params(new { text = "sheet" }));
        Assert.True(note2.RegenerateCommand.CanExecute(null));
        _ = engine;
        _ = session2;
    }

    [Fact]
    public async Task SaveSendsTheEditedDocuments()
    {
        var (session, engine, note) = await ReviewedSessionAsync();
        note.ClinicalNoteText = "edited note";
        note.PatientInfoText = "edited sheet";

        Assert.True(note.SaveNoteCommand.CanExecute(null));
        await note.SaveNoteCommand.ExecuteAsync(null);
        await note.SavePatientCommand.ExecuteAsync(null);

        var noteSave = engine.Requests.Single(r => r.Method == "note/update");
        Assert.Contains("s1", noteSave.Params);
        Assert.Contains("edited note", noteSave.Params);
        var patientSave = engine.Requests.Single(r => r.Method == "patient/update");
        Assert.Contains("edited sheet", patientSave.Params);
        Assert.Equal("Patient note saved", session.Status.LatestActivity);
    }

    [Fact]
    public async Task SaveIsHeldUntilTheDocumentIsSealed()
    {
        var (session, engine, note) = TestSession.Create();
        Assert.False(note.SaveNoteCommand.CanExecute(null));

        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        engine.RaiseNotification("note/partial", Params(new { text = "streaming" }));
        Assert.False(note.SaveNoteCommand.CanExecute(null));
        Assert.False(note.SavePatientCommand.CanExecute(null));

        engine.RaiseNotification("note/ready", Params(new { text = "the note" }));
        Assert.False(note.SaveNoteCommand.CanExecute(null));
        engine.RaiseNotification("patient/ready", Params(new { text = "the sheet" }));
        Assert.True(note.SaveNoteCommand.CanExecute(null));
        Assert.True(note.SavePatientCommand.CanExecute(null));
    }
}
