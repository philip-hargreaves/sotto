using Microsoft.Extensions.DependencyInjection;
using Microsoft.UI.Xaml;
using Sotto.App.Core;
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

        // Supervision replaces the fake with the pid-verified transport
        services.AddSingleton<IEngineClient, FakeEngineClient>();
        services.AddSingleton<IUiDispatcher, UiDispatcher>();

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
        _window = Services.GetRequiredService<MainWindow>();
        _window.Activate();
    }
}
