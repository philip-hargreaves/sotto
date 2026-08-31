using Ambient.App.Core;
using Ambient.App.Core.ViewModels;

namespace Ambient.App.Tests;

public class MicViewModelTest
{
    private static AppPreferences TempPreferences() =>
        new(Path.Combine(Path.GetTempPath(), Path.GetRandomFileName()));

    private static MicDevice Array(bool isDefault = true) =>
        new("{aa}", "Microphone Array (Cirrus Logic)", "Microphone Array", isDefault, false);

    private static MicDevice Jabra() =>
        new("{bb}", "Headset (Jabra Evolve2 65)", "Headset", false, true);

    [Fact]
    public async Task TheLabelNamesOneDeviceAsShortAsThatAllows()
    {
        var engine = new FakeEngineClient();
        var mic = new MicViewModel(engine);

        // A single-microphone laptop - the common clinical case - reads cleanly
        engine.AudioInputs = [Array()];
        await mic.RefreshAsync();
        Assert.Equal("Microphone Array", mic.Label);

        // Two devices, distinct endpoints: still just the endpoints
        engine.AudioInputs = [Array(), Jabra()];
        await mic.RefreshAsync();
        Assert.Equal("Microphone Array", mic.Label);

        // Three devices all called "Microphone": the full name disambiguates
        engine.AudioInputs =
        [
            new("{aa}", "Microphone (USB Audio)", "Microphone", true, false),
            new("{bb}", "Microphone (Jabra)", "Microphone", false, true),
            new("{cc}", "Microphone (Realtek)", "Microphone", false, false),
        ];
        await mic.RefreshAsync();
        Assert.Equal("Microphone (USB Audio)", mic.Label);
    }

    [Fact]
    public async Task TheChoicePersistsAndAGoneChoiceFallsToTheDefault()
    {
        var preferences = TempPreferences();
        var engine = new FakeEngineClient();
        var mic = new MicViewModel(engine, preferences);
        engine.AudioInputs = [Array(), Jabra()];
        await mic.RefreshAsync();

        mic.Select("{bb}");
        Assert.Equal("{bb}", mic.MicId);
        Assert.Equal("{bb}", preferences.MicId);

        // The headset is unplugged: the default speaks for it, but the
        // saved choice survives for when it comes back
        engine.AudioInputs = [Array()];
        await mic.RefreshAsync();
        Assert.Equal("{aa}", mic.MicId);
        Assert.Equal("{bb}", preferences.MicId);

        engine.AudioInputs = [Array(), Jabra()];
        await mic.RefreshAsync();
        Assert.Equal("{bb}", mic.MicId);
    }

    [Fact]
    public async Task NoMicrophoneSaysSoAndSendsNothing()
    {
        var engine = new FakeEngineClient();
        var mic = new MicViewModel(engine);
        await mic.RefreshAsync();

        Assert.False(mic.HasDevices);
        Assert.Equal("No microphone found", mic.Label);
        Assert.Equal("", mic.MicId);
    }

    [Fact]
    public async Task StartSendsTheSavedMicrophone()
    {
        var preferences = TempPreferences();
        preferences.MicId = "{bb}";
        var (session, engine, _) = TestSession.Create(preferences);

        await session.StartRecordingAsync();

        var start = engine.Requests.Single(r => r.Method == "session/start");
        Assert.Contains("{bb}", start.Params);
    }
}
