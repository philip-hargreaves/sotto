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
        ViewModel.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(NoteViewModel.PatientEditing))
            {
                ShowEditingChrome(PatientBox, ViewModel.PatientEditing);
            }

            if (e.PropertyName == nameof(NoteViewModel.TranslationVisible))
            {
                TranslationRow.Height = ViewModel.TranslationVisible
                    ? new GridLength(1, GridUnitType.Star)
                    : new GridLength(0);
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


    // The sheet and its translation travel together to the patient
    private async void OnExportPatient(object sender, RoutedEventArgs e)
    {
        var path = await SavePickerHelper.PickAsync("patient-sheet.txt", "Text file", ".txt");
        if (path is null)
        {
            return;
        }

        var text = ViewModel.PatientInfoText;
        if (ViewModel.TranslationText.Length > 0)
        {
            text += "\n\n" + ViewModel.TranslationCaption + "\n\n" + ViewModel.TranslationText;
        }

        await System.IO.File.WriteAllTextAsync(path, text);
        _status.Append($"Saved to {System.IO.Path.GetFileName(path)} - outside the encrypted store");
    }

    private async void OnCopyPatient(object sender, RoutedEventArgs e) =>
        await ClipboardHelper.CopyAsync(_status, ViewModel.PatientInfoText, "Patient note");
}
