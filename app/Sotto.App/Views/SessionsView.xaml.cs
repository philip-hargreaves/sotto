using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Sotto.App.Core.ViewModels;

namespace Sotto.App.Views;

public sealed partial class SessionsView : UserControl
{
    public SessionsView(SessionsViewModel viewModel, ShellViewModel shell)
    {
        ViewModel = viewModel;
        Shell = shell;
        InitializeComponent();
        Loaded += (_, _) => _ = ViewModel.RefreshAsync();
    }

    public SessionsViewModel ViewModel { get; }

    public ShellViewModel Shell { get; }

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
            Title = "Delete this session?",
            Content = "The recording and transcript are erased and cannot be recovered.",
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
