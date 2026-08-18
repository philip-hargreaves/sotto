using Microsoft.Extensions.DependencyInjection;
using Microsoft.UI.Xaml;
using Microsoft.Windows.Storage;
using Sotto.App.Core;
using Sotto.App.Core.Hosting;
using Sotto.App.Core.ViewModels;
using Sotto.App.Services;
using Sotto.App.Views;
using Sotto.Client;

namespace Sotto.App;

public partial class App : Application
{
    private Window? _window;

    public App()
    {
        InitializeComponent();
        Services = ConfigureServices();
    }

    public new static App Current => (App)Application.Current;

    public IServiceProvider Services { get; }

    /// <summary>The main window, for pickers that need an HWND.</summary>
    public Window? Window => _window;

    private static readonly TimeSpan EngineConnectTimeout = TimeSpan.FromSeconds(10);

    private static ServiceProvider ConfigureServices()
    {
        var services = new ServiceCollection();

        services.AddSingleton<IUiDispatcher, UiDispatcher>();

        services.AddSingleton(TimeProvider.System);
        services.AddSingleton<IEngineLauncher>(_ => new ProcessEngineLauncher(
            Path.Combine(AppContext.BaseDirectory, "sotto_engine.exe")));
        // Identity-free path: unpackaged runs have no ApplicationData
        var localState = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "sotto");
        services.AddSingleton<ICrashLog>(_ => new FileCrashLog(
            Path.Combine(localState, "crashes.jsonl")));
        services.AddSingleton<ISessionState>(sp => new DeferredSessionState(sp));
        services.AddSingleton<IEngineHost>(sp => new EngineSupervisor(
            sp.GetRequiredService<IEngineLauncher>(),
            sp.GetRequiredService<ISessionState>(),
            sp.GetRequiredService<TimeProvider>(),
            sp.GetRequiredService<ICrashLog>(),
            () => sp.GetRequiredService<EngineConnection>().MethodInFlight));
        services.AddSingleton(sp => new EngineConnection(
            sp.GetRequiredService<IEngineHost>(),
            static async (pid, ct) => await PipeTransport.ConnectAsync(
                EngineInfo.PipeName, EngineConnectTimeout, pid, ct).ConfigureAwait(false)));
        services.AddSingleton<IEngineClient>(sp => sp.GetRequiredService<EngineConnection>());

        services.AddSingleton<NavigationService>();
        services.AddSingleton<INavigationService>(sp => sp.GetRequiredService<NavigationService>());

        services.AddSingleton(_ => AppPreferences.Load(
            Path.Combine(localState, "preferences.json")));
        services.AddSingleton<TranscriptViewModel>();
        services.AddSingleton<NoteViewModel>();
        services.AddSingleton<StatusBarViewModel>();
        services.AddSingleton<ConsultationViewModel>();
        services.AddSingleton<SessionControlsViewModel>();
        services.AddSingleton<ShellViewModel>();
        services.AddSingleton<SettingsViewModel>();
        services.AddSingleton<SessionsViewModel>();
        services.AddSingleton<DemoTrayViewModel>();

        services.AddTransient<SessionControlsView>();
        services.AddTransient<DemoTrayView>();
        services.AddTransient<TranscriptPaneView>();
        services.AddTransient<NotePaneView>();
        services.AddTransient<StatusBarView>();
        services.AddTransient<ConsultationView>();
        services.AddTransient<SessionsView>();
        services.AddTransient<SettingsView>();
        services.AddTransient<MainWindow>();

        return services.BuildServiceProvider();
    }

    // The client, the host and the session state form a cycle, so the view
    // model behind ISessionState is resolved on first read, not at build
    private sealed class DeferredSessionState(IServiceProvider services) : ISessionState
    {
        public bool ConsultationActive =>
            services.GetRequiredService<ConsultationViewModel>().ConsultationActive;
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        // Subscribed here because a status-bar ctor dependency on the host
        // would close a DI cycle: host -> session state -> status bar
        var host = Services.GetRequiredService<IEngineHost>();
        var dispatcher = Services.GetRequiredService<IUiDispatcher>();
        var statusBar = Services.GetRequiredService<StatusBarViewModel>();
        host.StatusChanged += _ =>
            dispatcher.Post(() => statusBar.SetEngineState(host.Status, host.Fault));

        _window = Services.GetRequiredService<MainWindow>();
        _window.Closed += (_, _) => host.Shutdown();
        _window.Activate();
        host.Start();
        _ = RequestMicrophoneAccessAsync();
    }

    // Registers the app on the Settings microphone page and shows the system
    // prompt where Windows requires one. Enforcement is the engine's job: it
    // reads the toggle this page writes before every capture.
    private static async Task RequestMicrophoneAccessAsync()
    {
        if (!OperatingSystem.IsWindowsVersionAtLeast(10, 0, 18362))
        {
            return;
        }

        try
        {
            await Windows.Security.Authorization.AppCapabilityAccess.AppCapability
                .Create("microphone").RequestAccessAsync();
        }
        catch (Exception)
        {
            // The toggle stays wherever it was; the engine still honours it
        }
    }
}
