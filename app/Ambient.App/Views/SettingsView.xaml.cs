using Microsoft.UI.Xaml.Controls;
using Sotto.App.Core.ViewModels;

namespace Sotto.App.Views;

public sealed partial class SettingsView : UserControl
{
    public SettingsView(SettingsViewModel viewModel, ShellViewModel shell)
    {
        ViewModel = viewModel;
        Shell = shell;
        InitializeComponent();
        // Turning the history on accumulates patient records: confirmed,
        // never just toggled. Cancel is the safe default.
        viewModel.ConfirmKeepConsultations = async () =>
        {
            var dialog = new ContentDialog
            {
                XamlRoot = XamlRoot,
                Title = "Save consultation data?",
                Content = "Transcripts, notes and patient sheets will be stored encrypted on "
                    + "this device.\n\nContinue only if you have the necessary consent and approval.",
                PrimaryButtonText = "Turn on",
                CloseButtonText = "Cancel",
                DefaultButton = ContentDialogButton.Close,
            };
            return await dialog.ShowAsync() == ContentDialogResult.Primary;
        };
    }

    public SettingsViewModel ViewModel { get; }

    public ShellViewModel Shell { get; }
}
