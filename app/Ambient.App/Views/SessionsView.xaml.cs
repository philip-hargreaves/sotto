using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Sotto.App.Core.ViewModels;

namespace Sotto.App.Views;

public sealed partial class SessionsView : UserControl
{
    public SessionsView(
        SessionsViewModel viewModel, ShellViewModel shell,
        TranscriptPaneView transcript, NoteEditorView note, PatientEditorView patient)
    {
        ViewModel = viewModel;
        Shell = shell;
        InitializeComponent();
        TranscriptHost.Content = transcript;
        NoteHost.Content = note;
        PatientHost.Content = patient;
        Loaded += (_, _) => _ = ViewModel.RefreshAsync();
        ViewModel.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(SessionsViewModel.DetailOpen)
                or nameof(SessionsViewModel.EmptyBecauseOff))
            {
                Bindings.Update();
            }
        };
    }

    public SessionsViewModel ViewModel { get; }

    public ShellViewModel Shell { get; }

    public bool SelectHintVisible => !ViewModel.DetailOpen && !ViewModel.EmptyBecauseOff;

    // Leaving the page ends the review: edits saved, the engine told
    private async void OnBackClick(object sender, RoutedEventArgs e)
    {
        await ViewModel.LeaveAsync();
        Shell.GoBackCommand.Execute(null);
    }

    // Push the text first: the two-way binding's order against this handler
    // is not guaranteed
    private async void OnTitleCommitted(object sender, RoutedEventArgs e)
    {
        ViewModel.DetailTitle = ((TextBox)sender).Text;
        await ViewModel.RenameAsync();
    }

    // Deletion is crypto-erase, so the confirmation lives here, not in the VM
    private async void OnDeleteClick(object sender, RoutedEventArgs e)
    {
        if (ViewModel.Selected is null)
        {
            return;
        }

        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Delete this consultation?",
            Content = "The transcript, note and patient sheet are erased and cannot be recovered.",
            PrimaryButtonText = "Delete",
            CloseButtonText = "Keep",
            DefaultButton = ContentDialogButton.Close,
        };
        if (await dialog.ShowAsync() == ContentDialogResult.Primary)
        {
            await ViewModel.DeleteSelectedAsync();
        }
    }
}
