using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Ambient.App.Services;
using Windows.Graphics;

namespace Ambient.App;

public sealed partial class MainWindow : Window
{
    public MainWindow(NavigationService navigation)
    {
        InitializeComponent();

        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);

        // Unpackaged: the taskbar and title bar take the window icon,
        // not the exe icon
        AppWindow.SetIcon(System.IO.Path.Combine(
            System.AppContext.BaseDirectory, "Assets", "AppIcon.ico"));

        navigation.Attach(NavHost);
        navigation.NavigateTo("consultation");

        AppWindow.Resize(new SizeInt32(1280, 820));
        if (AppWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.PreferredMinimumWidth = 960;
            presenter.PreferredMinimumHeight = 640;
        }
    }
}
