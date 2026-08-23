using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace Sotto.App.Core.ViewModels;

public sealed partial class NoteViewModel : ObservableObject
{
    [ObservableProperty]
    public partial NotePipelineState PipelineState { get; private set; } = NotePipelineState.Pending;

    [ObservableProperty]
    public partial string ClinicalNoteText { get; set; } = "";

    [ObservableProperty]
    public partial string PatientInfoText { get; set; } = "";

    [ObservableProperty]
    public partial string TranslationText { get; set; } = "";

    public ObservableCollection<string> Languages { get; } = [];

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(TranslateCommand))]
    public partial string? SelectedLanguage { get; set; }

    /// <summary>Set by the consultation view model, which owns the engine.</summary>
    public Func<string, Task>? TranslateRequested { get; set; }

    /// <summary>True from the request until translate/ready or translate/failed.</summary>
    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(TranslateCommand))]
    public partial bool TranslationRunning { get; set; }

    [RelayCommand(CanExecute = nameof(CanTranslate))]
    private Task Translate()
    {
        TranslationRunning = true;
        return TranslateRequested!(SelectedLanguage!);
    }

    // The engine translates the stored sheet, which exists only once the
    // pipeline reports it ready - text alone streams in earlier
    private bool CanTranslate() => SelectedLanguage is not null && TranslateRequested is not null
        && PipelineState == NotePipelineState.AllReady && !TranslationRunning;

    // The panes show a quiet affordance while a document is being prepared
    // and nothing has streamed yet; computed here so it is testable
    public bool NotePreparing =>
        PipelineState == NotePipelineState.NoteWriting && ClinicalNoteText.Length == 0;

    public bool PatientPreparing =>
        PipelineState is NotePipelineState.NoteWriting or NotePipelineState.NoteReadyPatientWriting
        && PatientInfoText.Length == 0;

    public string NoteStateCaption => PipelineState switch
    {
        NotePipelineState.NoteWriting => "Writing the note",
        NotePipelineState.NoteFailed => "The note could not be written - see the status bar",
        _ => "",
    };

    public string PatientStateCaption => PipelineState switch
    {
        NotePipelineState.NoteWriting or NotePipelineState.NoteReadyPatientWriting =>
            "The information sheet follows the note",
        NotePipelineState.PatientFailed =>
            "The information sheet could not be written - see the status bar",
        _ => "",
    };

    public bool NoteCaptionVisible => NoteStateCaption.Length > 0;

    public bool PatientCaptionVisible => PatientStateCaption.Length > 0;

    // Translation appears only when it has a purpose: the row once the sheet
    // is stored, the output only while translating or holding a result
    public bool TranslateRowVisible => PipelineState == NotePipelineState.AllReady;

    public bool TranslationVisible => TranslationRunning || TranslationText.Length > 0;

    partial void OnPipelineStateChanged(NotePipelineState value)
    {
        TranslateCommand.NotifyCanExecuteChanged();
        OnPropertyChanged(nameof(NotePreparing));
        OnPropertyChanged(nameof(PatientPreparing));
        OnPropertyChanged(nameof(NoteStateCaption));
        OnPropertyChanged(nameof(PatientStateCaption));
        OnPropertyChanged(nameof(NoteCaptionVisible));
        OnPropertyChanged(nameof(PatientCaptionVisible));
        OnPropertyChanged(nameof(TranslateRowVisible));
    }

    partial void OnTranslationRunningChanged(bool value) =>
        OnPropertyChanged(nameof(TranslationVisible));

    partial void OnTranslationTextChanged(string value) =>
        OnPropertyChanged(nameof(TranslationVisible));

    partial void OnClinicalNoteTextChanged(string value) =>
        OnPropertyChanged(nameof(NotePreparing));

    partial void OnPatientInfoTextChanged(string value) =>
        OnPropertyChanged(nameof(PatientPreparing));

    /// <summary>Applies an engine-reported event; out-of-order events are refused.</summary>
    public bool Apply(NotePipelineEvent pipelineEvent)
    {
        var next = (PipelineState, pipelineEvent) switch
        {
            (NotePipelineState.Pending, NotePipelineEvent.NoteWritingStarted)
                => NotePipelineState.NoteWriting,
            (NotePipelineState.NoteWriting, NotePipelineEvent.NoteReady)
                => NotePipelineState.NoteReadyPatientWriting,
            (NotePipelineState.NoteWriting, NotePipelineEvent.NoteFailed)
                => NotePipelineState.NoteFailed,
            (NotePipelineState.NoteReadyPatientWriting, NotePipelineEvent.PatientInfoReady)
                => NotePipelineState.AllReady,
            (NotePipelineState.NoteReadyPatientWriting, NotePipelineEvent.PatientInfoFailed)
                => NotePipelineState.PatientFailed,
            _ => (NotePipelineState?)null,
        };
        if (next is null)
        {
            return false;
        }

        PipelineState = next.Value;
        return true;
    }

    public void Reset()
    {
        PipelineState = NotePipelineState.Pending;
        ClinicalNoteText = "";
        PatientInfoText = "";
        TranslationText = "";
        TranslationRunning = false;
    }
}
