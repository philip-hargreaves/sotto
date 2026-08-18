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

        Assert.Equal(
            ["doctor: how long has the knee been swollen", "patient: about three weeks now"],
            session.Transcript.Turns);
    }

    [Fact]
    public async Task AnUnlabelledTurnRendersAsBareText()
    {
        var (session, engine, _) = TestSession.Create();
        engine.Transcript.Add(("", "hello there"));

        await session.StartRecordingAsync();
        await session.StopRecordingAsync();

        Assert.Equal(["hello there"], session.Transcript.Turns);
    }
}
