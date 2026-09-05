using Ambient.App.Core.Hosting;
using Ambient.App.Core.ViewModels;

namespace Ambient.App.Tests;

public class VoiceViewModelTest
{
    private sealed class FakeSession : ISessionState
    {
        public bool ConsultationActive { get; set; }
    }

    [Fact]
    public async Task TheHeadlineNamesEachStateOfThePrint()
    {
        var engine = new FakeEngineClient();
        var voice = new VoiceViewModel(engine);

        await voice.RefreshAsync();
        Assert.False(voice.HasVoice);
        Assert.StartsWith("Tells you apart", voice.Headline);
        Assert.EndsWith("or set up now.", voice.Headline);
        Assert.Equal("Set up", voice.SetUpLabel);

        engine.AnchorOrigin = "accrued";
        engine.AnchorSessions = 3;
        await voice.RefreshAsync();
        Assert.Equal("Learning automatically, 3 consultations so far", voice.Headline);

        engine.AnchorSessions = 12;
        await voice.RefreshAsync();
        Assert.Equal("Learned automatically from 12 consultations", voice.Headline);
        Assert.Equal("Redo", voice.SetUpLabel);

        engine.AnchorOrigin = "enrolled";
        engine.AnchorSessions = 0;
        engine.AnchorEnrolledAt = new DateTimeOffset(2026, 9, 4, 14, 2, 0, TimeSpan.Zero)
            .ToUnixTimeSeconds();
        await voice.RefreshAsync();
        Assert.StartsWith("Set up on 4 Sep", voice.Headline);

        engine.AnchorSessions = 1;
        await voice.RefreshAsync();
        Assert.EndsWith("refined automatically by 1 consultation since", voice.Headline);
    }

    [Fact]
    public async Task ForgettingIsConfirmedThenClearsAndReportsBack()
    {
        var engine = new FakeEngineClient { AnchorOrigin = "accrued", AnchorSessions = 4 };
        var status = new StatusBarViewModel();
        var voice = new VoiceViewModel(engine, new FakeSession(), status);
        await voice.RefreshAsync();
        Assert.True(voice.ForgetVoiceCommand.CanExecute(null));

        voice.ConfirmForget = () => Task.FromResult(false);
        await voice.ForgetVoiceCommand.ExecuteAsync(null);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "anchor/clear");
        Assert.True(voice.HasVoice);

        voice.ConfirmForget = () => Task.FromResult(true);
        await voice.ForgetVoiceCommand.ExecuteAsync(null);
        Assert.Contains(engine.Requests, r => r.Method == "anchor/clear");
        Assert.False(voice.HasVoice);
        Assert.False(voice.ForgetVoiceCommand.CanExecute(null));
        Assert.Contains("forgotten", status.LatestActivity);
    }

    [Fact]
    public async Task NothingChangesDuringAConsultation()
    {
        var engine = new FakeEngineClient { AnchorOrigin = "accrued", AnchorSessions = 4 };
        var status = new StatusBarViewModel();
        var session = new FakeSession { ConsultationActive = true };
        var voice = new VoiceViewModel(engine, session, status)
        {
            ConfirmForget = () => Task.FromResult(true),
            RunEnrolment = () => Task.FromResult(true),
        };
        await voice.RefreshAsync();

        await voice.ForgetVoiceCommand.ExecuteAsync(null);
        await voice.SetUpVoiceCommand.ExecuteAsync(null);

        Assert.DoesNotContain(engine.Requests, r => r.Method == "anchor/clear");
        Assert.True(voice.HasVoice);
        Assert.Contains("finish the consultation", status.LatestActivity);
    }

    [Fact]
    public async Task SettingUpRunsTheDialogThenRereadsTheEngine()
    {
        var engine = new FakeEngineClient();
        var status = new StatusBarViewModel();
        var voice = new VoiceViewModel(engine, new FakeSession(), status);
        await voice.RefreshAsync();
        Assert.False(voice.SetUpVoiceCommand.CanExecute(null), "no dialog wired yet");

        voice.RunEnrolment = () =>
        {
            engine.AnchorOrigin = "enrolled";  // what the dialog's enrolment did
            engine.AnchorEnrolledAt = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
            return Task.FromResult(true);
        };
        voice.NotifyCommands();
        Assert.True(voice.SetUpVoiceCommand.CanExecute(null));
        await voice.SetUpVoiceCommand.ExecuteAsync(null);

        Assert.Equal("enrolled", voice.Origin);
        Assert.StartsWith("Set up on", voice.Headline);
        Assert.Contains("enrolment complete", status.LatestActivity);
        Assert.False(voice.Busy);
    }
}
