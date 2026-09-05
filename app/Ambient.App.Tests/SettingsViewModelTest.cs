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

    // ---- note model tier ---------------------------------------------------

    private static FakeEngineClient TieredEngine()
    {
        var engine = new FakeEngineClient();
        engine.ExtraNoteModels.Add(("qwen3.6-35b-a3b-int4", "Qwen3.6 35B", "accuracy"));
        engine.ExtraNoteModels.Add(("qwen3.5-4b-int4", "Qwen3.5 4B", "constrained"));
        return engine;
    }

    private static System.Text.Json.JsonElement NoteModel(
        string state, string tier, string name, string? detail = null) =>
        System.Text.Json.JsonSerializer.SerializeToElement(
            new { state, tier, name, id = tier, seconds = 12.0, firstUse = false, detail });

    [Fact]
    public void NoteModelsComeFromTheEngineInLadderOrderAndThePreferenceSelects()
    {
        var preferences = TempPreferences();
        preferences.NoteTier = "accuracy";
        var engine = TieredEngine();

        var settings = new SettingsViewModel(preferences, client: engine);

        Assert.Equal(["Qwen3.5 4B", "Qwen3.5 9B", "Qwen3.6 35B"], settings.NoteModelOptions);
        Assert.Equal(2, settings.NoteModelIndex);
        Assert.True(settings.NoteModelEnabled);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "note/tier");  // restoring is not choosing
    }

    // A reconnect re-reads the store; the same models must not rebuild the bound
    // collection under the control, only a changed store does
    [Fact]
    public void AReconnectWithTheSameModelsLeavesTheCollectionAlone()
    {
        var engine = TieredEngine();
        var settings = new SettingsViewModel(TempPreferences(), client: engine, session: new FakeSession());
        var changes = 0;
        settings.NoteModelOptions.CollectionChanged += (_, _) => changes++;
        settings.NoteModelIndex = 2;

        engine.SetConnected(false);
        engine.SetConnected(true);

        Assert.Equal(0, changes);
        Assert.Equal(2, settings.NoteModelIndex);

        engine.ExtraNoteModels.RemoveAt(1);  // the 4B was uninstalled
        engine.SetConnected(false);
        engine.SetConnected(true);

        Assert.True(changes > 0);
        Assert.Equal(["Qwen3.5 9B", "Qwen3.6 35B"], settings.NoteModelOptions);
        Assert.Equal(1, settings.NoteModelIndex);
    }

    [Fact]
    public void ASingleStagedModelLeavesNothingToChoose()
    {
        var settings = new SettingsViewModel(TempPreferences(), client: new FakeEngineClient());

        Assert.Equal(["Qwen3.5 9B"], settings.NoteModelOptions);
        Assert.Equal(0, settings.NoteModelIndex);
        Assert.False(settings.NoteModelEnabled);
        Assert.Equal("Only one note model is installed", settings.NoteModelStatus);
    }

    [Fact]
    public void APreferenceForAnUninstalledTierFallsBackToTheDefault()
    {
        var preferences = TempPreferences();
        preferences.NoteTier = "accuracy";

        var settings = new SettingsViewModel(preferences, client: new FakeEngineClient());

        Assert.Equal("default", preferences.NoteTier);
        Assert.Equal(0, settings.NoteModelIndex);
        Assert.Contains("not installed", settings.NoteModelStatus);
    }

    [Fact]
    public void ChoosingATierPersistsItConfiguresTheEngineAndGreysUntilReady()
    {
        var preferences = TempPreferences();
        var engine = TieredEngine();
        var settings = new SettingsViewModel(preferences, client: engine, session: new FakeSession());

        settings.NoteModelIndex = 2;

        Assert.Equal("accuracy", preferences.NoteTier);
        var request = engine.Requests.Single(r => r.Method == "note/tier");
        Assert.Contains("accuracy", request.Params);
        Assert.False(settings.NoteModelEnabled, "greyed while the lane loads");
        Assert.Contains("Loading Qwen3.6 35B", settings.NoteModelStatus);

        engine.RaiseNotification("note/model", NoteModel("loading", "accuracy", "Qwen3.6 35B"));
        Assert.False(settings.NoteModelEnabled);

        engine.RaiseNotification("note/model", NoteModel("ready", "accuracy", "Qwen3.6 35B"));
        Assert.True(settings.NoteModelEnabled);
        Assert.Equal("Qwen3.6 35B ready (12 s)", settings.NoteModelStatus);
        Assert.Equal(2, settings.NoteModelIndex);
    }

    [Fact]
    public void AFailedLoadRevertsToTheTierThatWorked()
    {
        var preferences = TempPreferences();
        var engine = TieredEngine();
        var settings = new SettingsViewModel(preferences, client: engine, session: new FakeSession());
        settings.NoteModelIndex = 2;
        engine.Requests.Clear();

        engine.RaiseNotification(
            "note/model", NoteModel("failed", "accuracy", "Qwen3.6 35B", "out of memory"));

        Assert.Equal("default", preferences.NoteTier);
        Assert.Equal(1, settings.NoteModelIndex);
        Assert.Contains("out of memory", settings.NoteModelStatus);
        var back = engine.Requests.Single(r => r.Method == "note/tier");
        Assert.Contains("default", back.Params);
    }

    [Fact]
    public void ARefusedRequestRevertsToo()
    {
        var preferences = TempPreferences();
        var engine = TieredEngine();
        var settings = new SettingsViewModel(preferences, client: engine, session: new FakeSession());
        engine.FailNext = method => method == "note/tier" ? new IOException("no such device") : null;

        settings.NoteModelIndex = 0;

        Assert.Equal("default", preferences.NoteTier);
        Assert.Equal(1, settings.NoteModelIndex);
        Assert.Contains("no such device", settings.NoteModelStatus);
    }

    [Fact]
    public void ChoosingATierRefusesDuringAConsultation()
    {
        var preferences = TempPreferences();
        var engine = TieredEngine();
        var session = new FakeSession { ConsultationActive = true };
        var settings = new SettingsViewModel(preferences, client: engine, session: session);

        settings.NoteModelIndex = 2;

        Assert.Equal(1, settings.NoteModelIndex);
        Assert.Equal("default", preferences.NoteTier);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "note/tier");
    }

    [Fact]
    public void TheEngineIsAuthoritativeAboutWhatIsResident()
    {
        var preferences = TempPreferences();
        var engine = TieredEngine();
        var settings = new SettingsViewModel(preferences, client: engine, session: new FakeSession());

        // Another shell instance, or the engine's own default: the control follows
        engine.RaiseNotification("note/model", NoteModel("ready", "constrained", "Qwen3.5 4B"));

        Assert.Equal(0, settings.NoteModelIndex);
        Assert.Equal("constrained", preferences.NoteTier);
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
    public async Task ExportGoesWhereThePickerChoseAndCancelIsSilent()
    {
        var dir = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        Directory.CreateDirectory(dir);
        var collector = new Ambient.App.Core.Metrics.PerformanceCollector(
            new FakeEngineClient(), () => true, () => null, Path.Combine(dir, "metrics.jsonl"));
        collector.SessionStarted("mic", 0, null);
        collector.StopRequested();
        await collector.SessionFinishedAsync(null, 10);
        var settings = new SettingsViewModel(machine: new FixedMachine(), metrics: collector);
        var chosen = Path.Combine(dir, "picked.html");

        settings.PickSavePath = _ => Task.FromResult<string?>(null);
        await settings.ExportPerformanceReportCommand.ExecuteAsync(null);
        Assert.Equal("", settings.ExportResult);
        Assert.False(File.Exists(chosen));

        settings.PickSavePath = _ => Task.FromResult<string?>(chosen);
        await settings.ExportPerformanceReportCommand.ExecuteAsync(null);
        Assert.True(File.Exists(chosen), "written where the picker chose");
        Assert.Contains("picked.html", settings.ExportResult);
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
