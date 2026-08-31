using Sotto.App.Core.ViewModels;

namespace Sotto.App.Tests;

public class FinalTranscriptTest
{
    [Fact]
    public async Task StopReplacesThePaneWithTheLabelledTranscript()
    {
        var (session, engine, _) = TestSession.Create();
        engine.Transcript.Add(("doctor", "how long has the knee been swollen"));
        engine.Transcript.Add(("patient", "about three weeks now"));

        await session.StartRecordingAsync();
        await session.StopRecordingAsync();

        Assert.Equal(2, session.Transcript.Turns.Count);
        Assert.Equal("doctor", session.Transcript.Turns[0].Speaker);
        Assert.Equal("how long has the knee been swollen", session.Transcript.Turns[0].Text);
        Assert.Equal("patient", session.Transcript.Turns[1].Speaker);
    }

    [Fact]
    public async Task LiveTurnsAreNotShown()
    {
        var (session, engine, _) = TestSession.Create();
        await session.StartRecordingAsync();

        engine.RaiseNotification("transcript.turn", System.Text.Json.JsonSerializer
            .SerializeToElement(new { firstFrame = 168000, frameCount = 16000, speaker = "", text = "hello there" }));

        Assert.Empty(session.Transcript.Turns);
    }

    [Fact]
    public void TimeLabelsAreMinutesAndSeconds()
    {
        var transcript = new TranscriptViewModel();
        transcript.Add("doctor", 16000UL * 605, "text");

        Assert.Equal("10:05", transcript.Turns[0].TimeLabel);
    }
}
