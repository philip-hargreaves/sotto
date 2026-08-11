using Microsoft.UI.Xaml.Controls;
using Sotto.App.Core.ViewModels;

namespace Sotto.App.Views;

public sealed partial class ConsultationView : UserControl
{
    public ConsultationView(
        ShellViewModel shell, SessionControlsView controls, TranscriptPaneView transcript,
        NotePaneView note, StatusBarView status)
    {
        Shell = shell;
        InitializeComponent();

        ControlsHost.Content = controls;
        TranscriptHost.Content = transcript;
        NoteHost.Content = note;
        StatusHost.Content = status;
    }

    public ShellViewModel Shell { get; }
}
