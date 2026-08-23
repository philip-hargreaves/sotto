using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Sotto.App.Core.ViewModels;

namespace Sotto.App.Views;

public sealed partial class ConsultationView : UserControl
{
    public ConsultationView(
        ShellViewModel shell, SessionControlsView controls, TranscriptPaneView transcript,
        NotePaneView note, StatusBarView status, DemoTrayView demoTray, SettingsViewModel settings)
    {
        Shell = shell;
        Controls = controls.ViewModel;
        InitializeComponent();

        ControlsHost.Content = controls;
        TranscriptHost.Content = transcript;
        NoteHost.Content = note;
        StatusHost.Content = status;
        DemoTrayHost.Content = demoTray;

        // The tray exists only while the settings toggle says so
        void Apply() => DemoTrayHost.Visibility =
            settings.DemoTrayEnabled ? Visibility.Visible : Visibility.Collapsed;
        Apply();
        settings.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(SettingsViewModel.DemoTrayEnabled))
            {
                Apply();
            }
        };
    }

    public ShellViewModel Shell { get; }

    public SessionControlsViewModel Controls { get; }
}
