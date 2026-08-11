using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Sotto.App.Services;
using Windows.Graphics;

namespace Sotto.App;

public sealed partial class MainWindow : Window
{
    public MainWindow(NavigationService navigation)
    {
        InitializeComponent();

        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);

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
