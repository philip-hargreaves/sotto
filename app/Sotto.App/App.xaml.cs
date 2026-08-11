using Microsoft.Extensions.DependencyInjection;
using Microsoft.UI.Xaml;
using Sotto.App.Core;
using Sotto.App.Core.ViewModels;
using Sotto.App.Services;
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

        services.AddSingleton<TranscriptViewModel>();
        services.AddSingleton<NoteViewModel>();
        services.AddSingleton<StatusBarViewModel>();
        services.AddSingleton<ConsultationViewModel>();
        services.AddSingleton<SessionControlsViewModel>();

        return services.BuildServiceProvider();
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        _window = new MainWindow();
        _window.Activate();
    }
}
