using System.Text.Json;
using Ambient.App.Core.ViewModels;

namespace Ambient.App.Tests;

public class EnrolmentViewModelTest
{
    private static JsonElement Params(object value) => JsonSerializer.SerializeToElement(value);

    [Fact]
    public async Task StartAsksTheEngineForTheReadingWindowOnTheChosenMicrophone()
    {
        var engine = new FakeEngineClient();
        using var enrolment = new EnrolmentViewModel(engine, micId: "mic-7", seconds: 30);
        Assert.Equal(EnrolmentState.Ready, enrolment.State);
        Assert.Equal("Start", enrolment.PrimaryText);
        Assert.Equal("Cancel", enrolment.CloseText);

        await enrolment.StartCommand.ExecuteAsync(null);

        var request = Assert.Single(engine.Requests, r => r.Method == "anchor/enrol");
        Assert.Contains("\"seconds\":30", request.Params);
        Assert.Contains("\"id\":\"mic-7\"", request.Params);
        Assert.Equal(EnrolmentState.Recording, enrolment.State);
        Assert.True(enrolment.Recording);
        Assert.Equal("Finish", enrolment.PrimaryText);
        Assert.False(enrolment.StartCommand.CanExecute(null));
        Assert.True(enrolment.CancelCommand.CanExecute(null));
        Assert.True(enrolment.FinishCommand.CanExecute(null));
    }

    [Fact]
    public async Task ProgressCountsClearSpeechThenFinishAndSuccessClose()
    {
        var engine = new FakeEngineClient();
        using var enrolment = new EnrolmentViewModel(engine);
        await enrolment.StartCommand.ExecuteAsync(null);

        engine.RaiseNotification("anchor/progress",
            Params(new { elapsed = 12.0, speech = 10.0, level = 0.7, clipped = false }));
        Assert.Equal(0.7, enrolment.Level);
        Assert.Equal(0.5, enrolment.Progress);
        Assert.False(enrolment.EnoughCaptured);
        Assert.StartsWith("Listening. Read to the end", enrolment.StatusLine);

        engine.RaiseNotification("anchor/progress",
            Params(new { elapsed = 30.0, speech = 24.0, level = 0.4, clipped = false }));
        Assert.Equal(1.0, enrolment.Progress);
        Assert.True(enrolment.EnoughCaptured);
        Assert.StartsWith("Enough captured", enrolment.StatusLine);

        await enrolment.FinishCommand.ExecuteAsync(null);
        Assert.Contains(engine.Requests, r => r.Method == "anchor/enrol/finish");

        engine.RaiseNotification("anchor/enrolled",
            Params(new { ok = true, detail = "", speechSeconds = 34.0 }));
        Assert.Equal(EnrolmentState.Succeeded, enrolment.State);
        Assert.Equal("Done", enrolment.PrimaryText);
        Assert.Equal("", enrolment.CloseText);
        Assert.Equal(0, enrolment.Level);
        Assert.True(await enrolment.Outcome);
    }

    [Fact]
    public async Task ARefusalSaysWhyAndOffersAnotherGo()
    {
        var engine = new FakeEngineClient();
        using var enrolment = new EnrolmentViewModel(engine);
        await enrolment.StartCommand.ExecuteAsync(null);

        engine.RaiseNotification("anchor/enrolled", Params(new
        {
            ok = false,
            detail = "not enough clear speech: 12 s of 20 s needed",
            speechSeconds = 12.0,
        }));

        Assert.Equal(EnrolmentState.Failed, enrolment.State);
        Assert.Contains("12 s of 20 s", enrolment.StatusLine);
        Assert.Equal("Try again", enrolment.PrimaryText);
        Assert.True(enrolment.StartCommand.CanExecute(null));

        await enrolment.StartCommand.ExecuteAsync(null);
        Assert.Equal(EnrolmentState.Recording, enrolment.State);
        Assert.Equal(2, engine.Requests.Count(r => r.Method == "anchor/enrol"));
    }

    [Fact]
    public async Task CancelAndDismissalReportNothingKept()
    {
        var engine = new FakeEngineClient();
        var enrolment = new EnrolmentViewModel(engine);
        await enrolment.StartCommand.ExecuteAsync(null);

        await enrolment.CancelCommand.ExecuteAsync(null);
        Assert.Contains(engine.Requests, r => r.Method == "anchor/enrol/cancel");

        enrolment.Dismiss();
        Assert.False(await enrolment.Outcome);
        enrolment.Dispose();
    }

    [Fact]
    public void ProgressBeforeStartIsIgnored()
    {
        var engine = new FakeEngineClient();
        using var enrolment = new EnrolmentViewModel(engine);
        engine.RaiseNotification("anchor/progress",
            Params(new { elapsed = 3.0, speech = 1.0, level = 0.9, clipped = true }));
        Assert.Equal(0, enrolment.Level);
        Assert.Equal(EnrolmentState.Ready, enrolment.State);
    }
}
