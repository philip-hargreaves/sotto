using Ambient.App.Core.ViewModels;
using Microsoft.UI.Xaml.Controls;

namespace Ambient.App.Views;

/// <summary>
/// The passage, the ring and the verdict. The primary button is Start, then
/// Finish, then Done or Try again; Cancel stops a reading and keeps nothing.
/// </summary>
public sealed partial class EnrolmentDialog : ContentDialog
{
    public EnrolmentDialog(EnrolmentViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
    }

    public EnrolmentViewModel ViewModel { get; }

    private async void OnPrimary(ContentDialog sender, ContentDialogButtonClickEventArgs args)
    {
        if (ViewModel.State == EnrolmentState.Succeeded)
        {
            return;  // Done: the dialog closes
        }

        args.Cancel = true;  // Start, Finish and Try again keep the dialog open
        if (ViewModel.State == EnrolmentState.Recording)
        {
            await ViewModel.FinishCommand.ExecuteAsync(null);
        }
        else
        {
            await ViewModel.StartCommand.ExecuteAsync(null);
        }
    }

    private void OnClose(ContentDialog sender, ContentDialogButtonClickEventArgs args)
    {
        ViewModel.Dismiss();
    }

    private void OnClosing(ContentDialog sender, ContentDialogClosingEventArgs args)
    {
        ViewModel.Dismiss();
    }
}
