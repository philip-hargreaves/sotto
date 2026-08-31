using Ambient.App.Core;
using Ambient.App.Core.Hosting;
using Ambient.App.Core.ViewModels;

namespace Ambient.App.Tests;

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
    public void RestoringSavedSettingsFiresNoHandlers()
    {
        var preferences = TempPreferences();
        preferences.NpuTranscription = true;
        preferences.KeepConsultations = true;
        var engine = new FakeEngineHost();
        var asked = 0;

        var settings = new SettingsViewModel(preferences, engine, new FakeSession());
        settings.ConfirmKeepConsultations = () =>
        {
            asked++;
            return Task.FromResult(false);
        };

        Assert.True(settings.NpuTranscription);
        Assert.True(settings.KeepConsultations);
        Assert.Empty(engine.Calls);  // a launch must never restart the engine
        Assert.Equal(0, asked);
    }

    [Fact]
    public void KeepConsultationsDefaultsOffAndPersists()
    {
        var preferences = TempPreferences();
        var settings = new SettingsViewModel(preferences);
        Assert.False(settings.KeepConsultations, "save nothing unless the clinician opts in");

        settings.KeepConsultations = true;  // no confirmer wired: acts directly
        Assert.True(preferences.KeepConsultations);

        var reopened = new SettingsViewModel(preferences);
        Assert.True(reopened.KeepConsultations);
    }

    [Fact]
    public async Task TurningOnIsConfirmedNeverJustToggled()
    {
        var preferences = TempPreferences();
        var settings = new SettingsViewModel(preferences);
        var asked = 0;
        var answer = false;
        settings.ConfirmKeepConsultations = () =>
        {
            asked++;
            return Task.FromResult(answer);
        };

        settings.KeepConsultations = true;
        await Task.Delay(20);
        Assert.Equal(1, asked);
        Assert.False(settings.KeepConsultations, "declined: the toggle stays off");
        Assert.False(preferences.KeepConsultations, "and nothing was persisted");

        answer = true;
        settings.KeepConsultations = true;
        await Task.Delay(20);
        Assert.Equal(2, asked);
        Assert.True(settings.KeepConsultations);
        Assert.True(preferences.KeepConsultations);

        settings.KeepConsultations = false;  // off is frictionless
        Assert.Equal(2, asked);
        Assert.False(preferences.KeepConsultations);
    }

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

    private sealed class FixedMachine : Ambient.App.Core.Metrics.IMachineInfoProvider
    {
        public Ambient.App.Core.Metrics.MachineInfo Describe() =>
            new("TestCpu", 32, "TestOs", [], null);
    }

    [Fact]
    public async Task ExportWritesTheHtmlReport()
    {
        var dir = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        Directory.CreateDirectory(dir);
        var collector = new Ambient.App.Core.Metrics.PerformanceCollector(
            new FakeEngineClient(), () => true, () => null, Path.Combine(dir, "metrics.jsonl"));
        collector.SessionStarted("mic", 0, null);
        collector.StopRequested();
        await collector.SessionFinishedAsync(null, 10);
        var settings = new SettingsViewModel(
            machine: new FixedMachine(), metrics: collector, exportDirectory: dir);

        settings.ExportPerformanceReportCommand.Execute(null);

        Assert.StartsWith("saved ", settings.ExportResult);
        var report = Directory.GetFiles(dir, "ambient-perf-*.html").Single();
        Assert.Contains("TestCpu", File.ReadAllText(report));
        Directory.Delete(dir, recursive: true);
    }

    [Fact]
    public void ExportWithoutDataExplainsItself()
    {
        var settings = new SettingsViewModel(machine: new FixedMachine());
        settings.ExportPerformanceReportCommand.Execute(null);
        Assert.Equal("no performance data collected yet", settings.ExportResult);
    }

    [Fact]
    public void PreferencesRoundTripThroughTheFile()
    {
        var path = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        var preferences = new AppPreferences(path)
        {
            NpuTranscription = true,
            ShowPerformanceMetrics = true,
        };
        preferences.Save();

        var loaded = AppPreferences.Load(path);
        Assert.True(loaded.NpuTranscription);
        Assert.True(loaded.ShowPerformanceMetrics);
        File.Delete(path);
    }

    [Fact]
    public void ThemeDefaultsToSystemPersistsAndAppliesLive()
    {
        var preferences = TempPreferences();
        var applied = new List<string>();
        var settings = new SettingsViewModel(preferences);
        settings.ApplyTheme = applied.Add;
        Assert.Equal("system", settings.Theme);
        Assert.Equal(0, settings.ThemeIndex);

        settings.ThemeIndex = 2;

        Assert.Equal("dark", settings.Theme);
        Assert.Equal(["dark"], applied);
        Assert.Equal("dark", preferences.Theme);

        var reopened = new SettingsViewModel(preferences);
        Assert.Equal(2, reopened.ThemeIndex);
    }

    [Fact]
    public void RestoringASavedThemeFiresNoHandlers()
    {
        var path = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        var preferences = new AppPreferences(path) { Theme = "light" };

        var settings = new SettingsViewModel(preferences);

        Assert.Equal("light", settings.Theme);
        Assert.False(File.Exists(path), "launch restore must not re-save");
    }

    [Fact]
    public void AnUnknownStoredThemeFallsToSystem()
    {
        var path = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        var preferences = new AppPreferences(path) { Theme = "dark" };
        preferences.Save();
        File.WriteAllText(path, File.ReadAllText(path).Replace("dark", "solarized"));

        Assert.Equal("system", AppPreferences.Load(path).Theme);
        File.Delete(path);
    }

    [Fact]
    public void MetricsToggleDefaultsOffAndReachesTheBar()
    {
        var preferences = TempPreferences();
        var bar = new StatusBarViewModel();
        var settings = new SettingsViewModel(preferences, status: bar);
        Assert.False(settings.ShowPerformanceMetrics, "chips are for testing, not GPs");
        Assert.False(bar.MetricsVisible);

        settings.ShowPerformanceMetrics = true;

        Assert.True(bar.MetricsVisible);
        Assert.True(preferences.ShowPerformanceMetrics);
    }
}
