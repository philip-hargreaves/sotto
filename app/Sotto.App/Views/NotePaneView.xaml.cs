using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Sotto.App.Core.ViewModels;

namespace Sotto.App.Views;

public sealed partial class NotePaneView : UserControl
{
    private readonly StatusBarViewModel _status;

    public NotePaneView(NoteViewModel viewModel, StatusBarViewModel status)
    {
        ViewModel = viewModel;
        _status = status;
        InitializeComponent();
        Select(StyleBox, ViewModel.Style);
        Select(DetailBox, ViewModel.Detail);
    }

    public NoteViewModel ViewModel { get; }

    // The view model speaks engine values, the combos display names; the
    // glue lives here with the values travelling as item tags
    private static void Select(ComboBox box, string tag) =>
        box.SelectedItem = box.Items.OfType<ComboBoxItem>()
            .FirstOrDefault(i => (string?)i.Tag == tag) ?? box.Items[0];

    private void OnStyleChanged(object sender, SelectionChangedEventArgs args)
    {
        if (StyleBox.SelectedItem is ComboBoxItem { Tag: string tag })
        {
            ViewModel.Style = tag;
        }
    }

    private void OnDetailChanged(object sender, SelectionChangedEventArgs args)
    {
        if (DetailBox.SelectedItem is ComboBoxItem { Tag: string tag })
        {
            ViewModel.Detail = tag;
        }
    }

    private async void OnCopyNote(object sender, RoutedEventArgs e) =>
        await CopyAsync(ViewModel.ClinicalNoteText, "Note");

    private async void OnCopyPatient(object sender, RoutedEventArgs e) =>
        await CopyAsync(ViewModel.PatientInfoText, "Patient note");

    // A fresh DataPackage per attempt is required: a package can be handed to
    // SetContent only once, so reuse across retries throws and leaves the
    // clipboard empty. Flush only decides whether the content outlives the
    // process, so its refusal must not fail a SetContent that succeeded.
    private async Task CopyAsync(string text, string what)
    {
        for (var attempt = 1; attempt <= 5; attempt++)
        {
            try
            {
                var data = new Windows.ApplicationModel.DataTransfer.DataPackage();
                data.SetText(text);
                Windows.ApplicationModel.DataTransfer.Clipboard.SetContent(data);
                try
                {
                    Windows.ApplicationModel.DataTransfer.Clipboard.Flush();
                }
                catch
                {
                }

                _status.Append($"{what} copied");
                return;
            }
            catch (Exception)
            {
                if (attempt == 5)
                {
                    _status.Append("Copy failed - the clipboard is unavailable");
                    return;
                }

                await Task.Delay(80 * attempt);
            }
        }
    }
}
