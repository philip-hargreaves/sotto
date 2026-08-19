using Sotto.App.Core;
using Sotto.App.Core.Hosting;
using Sotto.App.Core.ViewModels;

namespace Sotto.App.Tests;

public class SettingsViewModelTest
{
    private sealed class FakeEngineHost : IEngineHost
    {
        public event Action<EngineStatus>? StatusChanged;

        public EngineStatus Status { get; private set; }

        public EngineFault? Fault => null;

        public int? EnginePid => null;

        public List<string> Calls { get; } = [];

        public void Start()
        {
            Calls.Add("start");
            Status = EngineStatus.Running;
            StatusChanged?.Invoke(Status);
        }

        public void Shutdown()
        {
            Calls.Add("shutdown");
            Status = EngineStatus.Stopped;
            StatusChanged?.Invoke(Status);
        }
    }

    private sealed class FakeSession : ISessionState
    {
        public bool ConsultationActive { get; set; }
    }

    private static AppPreferences TempPreferences() =>
        new(Path.Combine(Path.GetTempPath(), Path.GetRandomFileName()));

    [Fact]
    public void NpuToggleSavesAndRestartsTheEngine()
    {
        var preferences = TempPreferences();
        var engine = new FakeEngineHost();
        var settings = new SettingsViewModel(preferences, engine, new FakeSession());

        settings.NpuTranscription = true;

        Assert.True(preferences.NpuTranscription);
        Assert.Equal(["shutdown", "start"], engine.Calls);
    }

    [Fact]
    public void NpuToggleRefusesDuringAConsultation()
    {
        var preferences = TempPreferences();
        var engine = new FakeEngineHost();
        var session = new FakeSession { ConsultationActive = true };
        var settings = new SettingsViewModel(preferences, engine, session);

        settings.NpuTranscription = true;

        Assert.False(settings.NpuTranscription);
        Assert.False(preferences.NpuTranscription);
        Assert.Empty(engine.Calls);
    }

    [Fact]
    public void PreferencesSeedTheToggles()
    {
        var preferences = TempPreferences();
        preferences.NpuTranscription = true;
        preferences.DemoTrayEnabled = true;

        var settings = new SettingsViewModel(preferences);

        Assert.True(settings.NpuTranscription);
        Assert.True(settings.DemoTrayEnabled);
    }

    [Fact]
    public void PreferencesRoundTripThroughTheFile()
    {
        var path = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        var preferences = new AppPreferences(path) { NpuTranscription = true };
        preferences.Save();

        Assert.True(AppPreferences.Load(path).NpuTranscription);
        File.Delete(path);
    }
}
