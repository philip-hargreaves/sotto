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
