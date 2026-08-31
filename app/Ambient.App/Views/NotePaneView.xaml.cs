using Microsoft.UI.Xaml.Controls;
using Sotto.App.Core.ViewModels;

namespace Sotto.App.Views;

/// <summary>
/// The consultation's right pane: the two editors in tabs. The editors are
/// their own controls so the Sessions view shows the same ones.
/// </summary>
public sealed partial class NotePaneView : UserControl
{
    public NotePaneView(NoteViewModel viewModel, NoteEditorView note, PatientEditorView patient)
    {
        ViewModel = viewModel;
        InitializeComponent();
        NoteHost.Content = note;
        PatientHost.Content = patient;
    }

    public NoteViewModel ViewModel { get; }
}
