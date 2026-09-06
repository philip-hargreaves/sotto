using Microsoft.UI.Xaml.Controls;
using Ambient.App.Core;
using Ambient.App.Core.ViewModels;
using Ambient.Client;

namespace Ambient.App.Views;

public sealed partial class SettingsView : UserControl
{
    public SettingsView(SettingsViewModel viewModel, ShellViewModel shell, VoiceViewModel voice,
        IEngineClient engine, MicViewModel mic, IUiDispatcher dispatcher)
    {
        ViewModel = viewModel;
        Shell = shell;
        Voice = voice;
        InitializeComponent();
        // Forgetting the voiceprint cannot be undone, so it is asked once
        voice.ConfirmForget = async () =>
        {
            var dialog = new ContentDialog
            {
                XamlRoot = XamlRoot,
                Title = "Forget voice enrolment?",
                Content = "It will be learned again from your next consultation.",
                PrimaryButtonText = "Forget",
                CloseButtonText = "Cancel",
                DefaultButton = ContentDialogButton.Close,
            };
            return await dialog.ShowAsync() == ContentDialogResult.Primary;
        };
        // One reading per dialog; the outcome says whether a print was kept
        voice.RunEnrolment = async () =>
        {
            using var enrolment = new EnrolmentViewModel(engine, mic.MicId, dispatcher: dispatcher);
            var dialog = new EnrolmentDialog(enrolment) { XamlRoot = XamlRoot };
            await dialog.ShowAsync();
            return await enrolment.Outcome;
        };
        voice.NotifyCommands();
        Loaded += (_, _) => _ = voice.RefreshAsync();
        // Turning the history on accumulates patient records: confirmed,
        // never just toggled. Cancel is the safe default.
        viewModel.PickSavePath = suggested =>
            SavePickerHelper.PickAsync(suggested, "HTML report", ".html");
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

    public VoiceViewModel Voice { get; }
}
