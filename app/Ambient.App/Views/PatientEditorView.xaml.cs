using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Ambient.App.Core.ViewModels;

namespace Ambient.App.Views;

public sealed partial class PatientEditorView : UserControl
{
    private readonly StatusBarViewModel _status;

    public PatientEditorView(NoteViewModel viewModel, StatusBarViewModel status)
    {
        ViewModel = viewModel;
        _status = status;
        InitializeComponent();
    }

    public NoteViewModel ViewModel { get; }

    private async void OnCopyPatient(object sender, RoutedEventArgs e) =>
        await ClipboardHelper.CopyAsync(_status, ViewModel.PatientInfoText, "Patient note");
}
