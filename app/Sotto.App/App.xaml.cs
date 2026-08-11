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

    private static ServiceProvider ConfigureServices()
    {
        var services = new ServiceCollection();

        // The live-transport PR swaps the fake for the pid-verified client
        services.AddSingleton<IEngineClient, FakeEngineClient>();
        services.AddSingleton<IUiDispatcher, UiDispatcher>();

        services.AddSingleton(TimeProvider.System);
        services.AddSingleton<IEngineLauncher>(_ => new ProcessEngineLauncher(
            Path.Combine(AppContext.BaseDirectory, "sotto_engine.exe")));
        services.AddSingleton<ICrashLog>(_ => new FileCrashLog(
            Path.Combine(ApplicationData.GetDefault().LocalPath, "crashes.jsonl")));
        services.AddSingleton<ISessionState>(sp => sp.GetRequiredService<ConsultationViewModel>());
        services.AddSingleton<IEngineHost, EngineSupervisor>();

        services.AddSingleton<NavigationService>();
        services.AddSingleton<INavigationService>(sp => sp.GetRequiredService<NavigationService>());

        services.AddSingleton<TranscriptViewModel>();
        services.AddSingleton<NoteViewModel>();
        services.AddSingleton<StatusBarViewModel>();
        services.AddSingleton<ConsultationViewModel>();
        services.AddSingleton<SessionControlsViewModel>();
        services.AddSingleton<ShellViewModel>();
        services.AddSingleton<SettingsViewModel>();

        services.AddTransient<SessionControlsView>();
        services.AddTransient<TranscriptPaneView>();
        services.AddTransient<NotePaneView>();
        services.AddTransient<StatusBarView>();
        services.AddTransient<ConsultationView>();
        services.AddTransient<SettingsView>();
        services.AddTransient<MainWindow>();

        return services.BuildServiceProvider();
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
    }
}
