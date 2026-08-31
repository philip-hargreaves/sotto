using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Ambient.App.Core.ViewModels;

namespace Ambient.App.Views;

public sealed partial class NoteEditorView : UserControl
{
    private readonly StatusBarViewModel _status;

    public NoteEditorView(NoteViewModel viewModel, StatusBarViewModel status)
    {
        ViewModel = viewModel;
        _status = status;
        InitializeComponent();
        Select(StyleBox, ViewModel.Style);
        Select(DetailBox, ViewModel.Detail);
        // A stored session brings its own options; the combos follow
        ViewModel.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(NoteViewModel.Style))
            {
                Select(StyleBox, ViewModel.Style);
            }
            else if (e.PropertyName == nameof(NoteViewModel.Detail))
            {
                Select(DetailBox, ViewModel.Detail);
            }
        };
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
        await ClipboardHelper.CopyAsync(_status, ViewModel.ClinicalNoteText, "Note");
}
