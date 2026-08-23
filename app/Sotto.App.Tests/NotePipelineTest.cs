using Sotto.App.Core.ViewModels;

namespace Sotto.App.Tests;

public class NotePipelineTest
{
    [Fact]
    public void HappySequenceAdvancesToAllReady()
    {
        var note = new NoteViewModel();

        Assert.True(note.Apply(NotePipelineEvent.NoteWritingStarted));
        Assert.True(note.Apply(NotePipelineEvent.NoteReady));
        Assert.Equal(NotePipelineState.NoteReadyPatientWriting, note.PipelineState);
        Assert.True(note.Apply(NotePipelineEvent.PatientInfoReady));
        Assert.Equal(NotePipelineState.AllReady, note.PipelineState);
    }

    [Theory]
    [InlineData(NotePipelineEvent.NoteReady)]
    [InlineData(NotePipelineEvent.PatientInfoReady)]
    public void OutOfOrderEventsAreRefusedFromPending(NotePipelineEvent pipelineEvent)
    {
        var note = new NoteViewModel();

        Assert.False(note.Apply(pipelineEvent));
        Assert.Equal(NotePipelineState.Pending, note.PipelineState);
    }

    [Fact]
    public void PatientReadyBeforeNoteReadyIsRefused()
    {
        var note = new NoteViewModel();
        note.Apply(NotePipelineEvent.NoteWritingStarted);

        Assert.False(note.Apply(NotePipelineEvent.PatientInfoReady));
        Assert.Equal(NotePipelineState.NoteWriting, note.PipelineState);
    }

    [Fact]
    public async Task ReviewBeginsWhileThePatientLeafletIsStillWriting()
    {
        var (session, engine, note) = TestSession.Create();
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();

        engine.RaiseNotification("note/ready");

        Assert.Equal(SessionState.Review, session.State);
        Assert.Equal(NotePipelineState.NoteReadyPatientWriting, note.PipelineState);

        engine.RaiseNotification("patient/ready");
        Assert.Equal(NotePipelineState.AllReady, note.PipelineState);
    }

    [Fact]
    public async Task PartialsStreamIntoTheNotePaneAndReadySeals()
    {
        var (session, engine, note) = TestSession.Create();
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();

        engine.RaiseNotification("note/partial", System.Text.Json.JsonSerializer
            .SerializeToElement(new { text = "The patient" }));
        Assert.Equal("The patient", note.ClinicalNoteText);

        engine.RaiseNotification("note/partial", System.Text.Json.JsonSerializer
            .SerializeToElement(new { text = "The patient presents" }));
        engine.RaiseNotification("note/ready", System.Text.Json.JsonSerializer
            .SerializeToElement(new { text = "The patient presents with bursitis." }));

        Assert.Equal("The patient presents with bursitis.", note.ClinicalNoteText);
        Assert.Equal(SessionState.Review, session.State);
    }

    [Fact]
    public async Task AFailedNoteStillReachesReview()
    {
        var (session, engine, note) = TestSession.Create();
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();

        engine.RaiseNotification("note/failed", System.Text.Json.JsonSerializer
            .SerializeToElement(new { detail = "the transcript is empty" }));

        Assert.Equal(SessionState.Review, session.State);
        Assert.Equal(NotePipelineState.NoteFailed, note.PipelineState);
    }

    [Fact]
    public async Task PatientInformationStreamsAndSeals()
    {
        var (session, engine, note) = TestSession.Create();
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        engine.RaiseNotification("note/ready", System.Text.Json.JsonSerializer
            .SerializeToElement(new { text = "the note" }));

        engine.RaiseNotification("patient/partial", System.Text.Json.JsonSerializer
            .SerializeToElement(new { text = "Your appointment" }));
        Assert.Equal("Your appointment", note.PatientInfoText);

        engine.RaiseNotification("patient/ready", System.Text.Json.JsonSerializer
            .SerializeToElement(new { text = "Your appointment today ..." }));
        Assert.Equal("Your appointment today ...", note.PatientInfoText);
        Assert.Equal(NotePipelineState.AllReady, note.PipelineState);
    }

    [Fact]
    public async Task AFailedPatientSheetHasItsOwnState()
    {
        var (session, engine, note) = TestSession.Create();
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        engine.RaiseNotification("note/ready", System.Text.Json.JsonSerializer
            .SerializeToElement(new { text = "the note" }));

        engine.RaiseNotification("patient/failed", System.Text.Json.JsonSerializer
            .SerializeToElement(new { detail = "generation failed" }));

        Assert.Equal(NotePipelineState.PatientFailed, note.PipelineState);
        Assert.Equal(SessionState.Review, session.State);
    }

    [Fact]
    public async Task TranslationFlowsThroughTheEngine()
    {
        var (session, engine, note) = TestSession.Create();
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        engine.RaiseNotification("note/ready", System.Text.Json.JsonSerializer
            .SerializeToElement(new { text = "the note" }));
        engine.RaiseNotification("patient/ready", System.Text.Json.JsonSerializer
            .SerializeToElement(new { text = "the sheet" }));

        Assert.Contains("Polish", note.Languages);
        note.SelectedLanguage = "Polish";
        Assert.True(note.TranslateCommand.CanExecute(null));
        await note.TranslateCommand.ExecuteAsync(null);

        var request = engine.Requests.Single(r => r.Method == "patient/translate");
        Assert.Contains("Polish", request.Params);
        Assert.Contains("s1", request.Params);

        engine.RaiseNotification("translate/partial", System.Text.Json.JsonSerializer
            .SerializeToElement(new { text = "Twoja" }));
        engine.RaiseNotification("translate/ready", System.Text.Json.JsonSerializer
            .SerializeToElement(new { text = "Twoja wizyta", language = "Polish" }));
        Assert.Equal("Twoja wizyta", note.TranslationText);
    }

    [Fact]
    public async Task TranslationWaitsForTheStoredSheet()
    {
        var (session, engine, note) = TestSession.Create();
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        engine.RaiseNotification("note/ready", System.Text.Json.JsonSerializer
            .SerializeToElement(new { text = "the note" }));
        note.SelectedLanguage = "French";

        // The sheet is still streaming: text exists in the pane, but the
        // engine has stored nothing yet - a request now would error
        engine.RaiseNotification("patient/partial", System.Text.Json.JsonSerializer
            .SerializeToElement(new { text = "Your appointment" }));
        Assert.False(note.TranslateCommand.CanExecute(null));

        engine.RaiseNotification("patient/ready", System.Text.Json.JsonSerializer
            .SerializeToElement(new { text = "Your appointment today ..." }));
        Assert.True(note.TranslateCommand.CanExecute(null));
    }

    [Fact]
    public async Task ASecondTranslationWaitsForTheFirst()
    {
        var (session, engine, note) = TestSession.Create();
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        engine.RaiseNotification("note/ready");
        engine.RaiseNotification("patient/ready", System.Text.Json.JsonSerializer
            .SerializeToElement(new { text = "the sheet" }));
        note.SelectedLanguage = "French";

        await note.TranslateCommand.ExecuteAsync(null);
        Assert.False(note.TranslateCommand.CanExecute(null));

        engine.RaiseNotification("translate/ready", System.Text.Json.JsonSerializer
            .SerializeToElement(new { text = "Votre rendez-vous", language = "French" }));
        Assert.True(note.TranslateCommand.CanExecute(null));
    }

    [Fact]
    public async Task AFailedTranslationReleasesTheButton()
    {
        var (session, engine, note) = TestSession.Create();
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        engine.RaiseNotification("note/ready");
        engine.RaiseNotification("patient/ready", System.Text.Json.JsonSerializer
            .SerializeToElement(new { text = "the sheet" }));
        note.SelectedLanguage = "French";

        await note.TranslateCommand.ExecuteAsync(null);
        engine.RaiseNotification("translate/failed", System.Text.Json.JsonSerializer
            .SerializeToElement(new { detail = "boom" }));

        Assert.True(note.TranslateCommand.CanExecute(null));
    }

    [Fact]
    public void PreparingShowsUntilTheFirstWordsStream()
    {
        var note = new NoteViewModel();
        Assert.False(note.NotePreparing);

        note.Apply(NotePipelineEvent.NoteWritingStarted);
        Assert.True(note.NotePreparing);
        Assert.True(note.PatientPreparing);
        Assert.Equal("Writing the note...", note.NoteStateCaption);

        note.ClinicalNoteText = "The patient";
        Assert.False(note.NotePreparing);
        Assert.True(note.PatientPreparing);

        note.Apply(NotePipelineEvent.NoteReady);
        note.PatientInfoText = "Your appointment";
        Assert.False(note.PatientPreparing);
    }

    [Fact]
    public void AFailedNoteCaptionPointsAtTheStatusBar()
    {
        var note = new NoteViewModel();
        note.Apply(NotePipelineEvent.NoteWritingStarted);
        note.Apply(NotePipelineEvent.NoteFailed);

        Assert.False(note.NotePreparing);
        Assert.Contains("could not be written", note.NoteStateCaption);
        Assert.True(note.NoteCaptionVisible);
    }

    [Fact]
    public async Task NewConsultationResetsThePipeline()
    {
        var (session, engine, note) = TestSession.Create();
        await session.StartRecordingAsync();
        await session.StopRecordingAsync();
        engine.RaiseNotification("note/ready");
        engine.RaiseNotification("patient/ready");

        session.StartNewConsultation();

        Assert.Equal(NotePipelineState.Pending, note.PipelineState);
    }
}
