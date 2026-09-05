using System.Text.Json;
using Ambient.App.Core.ViewModels;

namespace Ambient.App.Tests;

public class NoteRefusalTest
{
    private static JsonElement Params(object value) => JsonSerializer.SerializeToElement(value);

    [Fact]
    public async Task ARefusalStaysOnTheRecordScreenSaysWhyAndOffersToWriteAnyway()
    {
        var (session, engine, note) = TestSession.Create();
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        engine.RaiseNotification("session/progress", Params(new { stage = "transcript" }));
        engine.RaiseNotification("note/refused", Params(new { reason = "a cooking video" }));

        Assert.Equal(NotePipelineState.NoteRefused, note.PipelineState);
        Assert.True(note.NoteRefused);
        Assert.Equal("a cooking video", note.RefusalReason);
        Assert.Equal(SessionState.Refused, session.State);
        Assert.StartsWith("No note", session.Status.LatestActivity);
        Assert.True(note.CanWriteAnyway);
        Assert.True(note.WriteAnywayCommand.CanExecute(null));

        await note.WriteAnywayCommand.ExecuteAsync(null);

        var request = Assert.Single(engine.Requests, r => r.Method == "note/regenerate");
        Assert.Contains("\"confirmed\":true", request.Params);
        Assert.Equal(SessionState.Review, session.State);
        Assert.Equal(NotePipelineState.NoteWriting, note.PipelineState);
        Assert.Equal("", note.RefusalReason);
        Assert.False(note.NoteRefused);
    }

    [Fact]
    public async Task DoneAfterARefusalClosesTheSessionAndReturnsToIdle()
    {
        var (session, engine, note) = TestSession.Create();
        var controls = new SessionControlsViewModel(session);
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        engine.RaiseNotification("session/progress", Params(new { stage = "transcript" }));
        engine.RaiseNotification("note/refused", Params(new { reason = "a cooking video" }));

        Assert.True(controls.RefusedVisible);
        Assert.True(controls.CentreStageVisible);
        Assert.True(controls.DoneCommand.CanExecute(null));

        await controls.DoneCommand.ExecuteAsync(null);

        Assert.Contains(engine.Requests, r => r.Method == "session/close");
        Assert.Equal(SessionState.Idle, session.State);
        Assert.Equal("", note.RefusalReason);
        Assert.False(controls.RefusedVisible);
    }

    [Fact]
    public async Task ATooShortRecordingIsRefusedWithoutTheOverride()
    {
        var (session, engine, note) = TestSession.Create();
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        engine.RaiseNotification("session/progress", Params(new { stage = "transcript" }));
        engine.RaiseNotification("note/refused",
            Params(new { reason = "12 words; a note needs at least 25", overridable = false }));

        Assert.True(note.NoteRefused);
        Assert.Equal("12 words; a note needs at least 25", note.RefusalReason);
        Assert.False(note.CanWriteAnyway);
        Assert.False(note.WriteAnywayCommand.CanExecute(null));
        Assert.Equal(SessionState.Refused, session.State);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "note/regenerate");
    }
}
