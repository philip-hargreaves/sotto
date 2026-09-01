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
            if (e.PropertyName == nameof(NoteViewModel.NoteEditing))
            {
                ShowEditingChrome(NoteBox, ViewModel.NoteEditing);
            }
            else if (e.PropertyName == nameof(NoteViewModel.Style))
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

    // The mode must be visible: an accent border while editing, the flat
    // document look otherwise
    private static void ShowEditingChrome(TextBox box, bool editing)
    {
        if (editing)
        {
            box.BorderBrush = (Microsoft.UI.Xaml.Media.Brush)
                Microsoft.UI.Xaml.Application.Current.Resources["AccentFillColorDefaultBrush"];
            box.BorderThickness = new Thickness(1.5);
        }
        else
        {
            box.ClearValue(Control.BorderBrushProperty);
            box.ClearValue(Control.BorderThicknessProperty);
        }
    }


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

    // Export is the one action that writes outside the encrypted store
    private async void OnExportNote(object sender, RoutedEventArgs e)
    {
        var path = await SavePickerHelper.PickAsync("clinical-note.txt", "Text file", ".txt");
        if (path is null)
        {
            return;
        }

        await System.IO.File.WriteAllTextAsync(path, ViewModel.ClinicalNoteText);
        _status.Append($"Saved to {System.IO.Path.GetFileName(path)} - outside the encrypted store");
    }

    private async void OnCopyNote(object sender, RoutedEventArgs e) =>
        await ClipboardHelper.CopyAsync(_status, ViewModel.ClinicalNoteText, "Note");
}
