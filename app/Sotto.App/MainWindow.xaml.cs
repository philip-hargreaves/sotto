using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Sotto.App.Views;
using Windows.Graphics;

namespace Sotto.App;

public sealed partial class MainWindow : Window
{
    public MainWindow(
        SessionControlsView controls, TranscriptPaneView transcript,
        NotePaneView note, StatusBarView status)
    {
        InitializeComponent();

        ControlsHost.Content = controls;
        TranscriptHost.Content = transcript;
        NoteHost.Content = note;
        StatusHost.Content = status;

        AppWindow.Resize(new SizeInt32(1280, 820));
        if (AppWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.PreferredMinimumWidth = 960;
            presenter.PreferredMinimumHeight = 640;
        }
    }
}
