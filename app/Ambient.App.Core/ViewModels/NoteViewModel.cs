using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace Ambient.App.Core.ViewModels;

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

    /// <summary>What the translation is, e.g. "Polish"; heads the output box.</summary>
    [ObservableProperty]
    public partial string TranslationLanguage { get; set; } = "";

    public string TranslationCaption =>
        TranslationLanguage.Length > 0 ? $"{TranslationLanguage} translation" : "Translation";

    partial void OnTranslationLanguageChanged(string value) =>
        OnPropertyChanged(nameof(TranslationCaption));

    /// <summary>Note options as the engine names them: "prose" or "soap".</summary>
    [ObservableProperty]
    public partial string Style { get; set; } = "prose";

    /// <summary>"concise", "standard" or "detailed".</summary>
    [ObservableProperty]
    public partial string Detail { get; set; } = "standard";

    public ObservableCollection<string> Languages { get; } = [];

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(TranslateCommand))]
    public partial string? SelectedLanguage { get; set; }

    /// <summary>Set by the consultation view model, which owns the engine.</summary>
    public Func<string, Task>? TranslateRequested { get; set; }

    public Func<Task>? RegenerateRequested { get; set; }

    public Func<Task>? SaveNoteRequested { get; set; }

    public Func<Task>? SavePatientRequested { get; set; }

    /// <summary>Raised when style or detail changes, for persistence.</summary>
    public Action? OptionsChanged { get; set; }

    /// <summary>True from the request until translate/ready or translate/failed.</summary>
    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(TranslateCommand))]
    public partial bool TranslationRunning { get; set; }

    /// <summary>"Edited 10:31" when a person changed the stored note; empty otherwise.</summary>
    [ObservableProperty]
    public partial string EditedStamp { get; set; } = "";

    /// <summary>
    /// The note has been edited since the sheet was written from it, so the
    /// sheet may no longer say what the note says. Cleared when the sheet is
    /// rewritten; the fix lands with the sheet-grounding work.
    /// </summary>
    [ObservableProperty]
    public partial bool PatientStale { get; set; }

    /// <summary>Open while Regenerate waits for the clinician to confirm losing an edit.</summary>
    [ObservableProperty]
    public partial bool RegenerateWarningOpen { get; set; }

    public bool Edited => EditedStamp.Length > 0;

    partial void OnEditedStampChanged(string value) => OnPropertyChanged(nameof(Edited));

    [RelayCommand]
    private async Task ConfirmRegenerate()
    {
        RegenerateWarningOpen = false;
        if (RegenerateRequested is not null)
        {
            await RegenerateRequested().ConfigureAwait(true);
        }
    }

    [RelayCommand]
    private void KeepEdits() => RegenerateWarningOpen = false;

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

    // An edited note is the clinician's wording; regenerating replaces it,
    // so it asks first. An unedited note regenerates straight away.
    [RelayCommand(CanExecute = nameof(CanRegenerate))]
    private Task Regenerate()
    {
        if (Edited)
        {
            RegenerateWarningOpen = true;
            return Task.CompletedTask;
        }

        return RegenerateRequested!();
    }

    // Any settled review state can regenerate, including a failed note -
    // regenerating IS the recovery. The engine refuses what it cannot do.
    private bool CanRegenerate() => RegenerateRequested is not null && PipelineState
        is NotePipelineState.AllReady or NotePipelineState.PatientFailed
        or NotePipelineState.NoteFailed;

    [RelayCommand(CanExecute = nameof(CanSaveNote))]
    private Task SaveNote() => SaveNoteRequested!();

    private bool CanSaveNote() => SaveNoteRequested is not null && NoteDocumentReady;

    [RelayCommand(CanExecute = nameof(CanSavePatient))]
    private Task SavePatient() => SavePatientRequested!();

    private bool CanSavePatient() => SavePatientRequested is not null && PatientDocumentReady;

    /// <summary>True once the document is sealed; gates save and copy.</summary>
    public bool NoteDocumentReady =>
        PipelineState is NotePipelineState.AllReady or NotePipelineState.PatientFailed;

    public bool PatientDocumentReady => PipelineState == NotePipelineState.AllReady;

    /// <summary>The pane returns to its writing look for a rewrite.</summary>
    public void BeginRegenerate()
    {
        PipelineState = NotePipelineState.NoteWriting;
        ClinicalNoteText = "";
        PatientInfoText = "";
        TranslationText = "";
        TranslationLanguage = "";
        TranslationRunning = false;
        EditedStamp = "";  // the rewrite replaces the edit
        PatientStale = false;
    }

    /// <summary>
    /// A stored session's documents, ready for review. The options show the
    /// stored values without becoming the clinician's new defaults.
    /// </summary>
    public void LoadStored(
        string note, string patient, string translation,
        string style, string detail, string editedStamp,
        string translationLanguage = "")
    {
        TranslationLanguage = translationLanguage;
        _suppressOptionsChanged = true;
        try
        {
            if (style.Length > 0)
            {
                Style = style;
            }

            if (detail.Length > 0)
            {
                Detail = detail;
            }
        }
        finally
        {
            _suppressOptionsChanged = false;
        }

        ClinicalNoteText = note;
        PatientInfoText = patient;
        TranslationText = translation;
        TranslationRunning = false;
        EditedStamp = editedStamp;
        RegenerateWarningOpen = false;
        PipelineState = NotePipelineState.AllReady;
    }

    private bool _suppressOptionsChanged;

    partial void OnStyleChanged(string value)
    {
        if (!_suppressOptionsChanged)
        {
            OptionsChanged?.Invoke();
        }
    }

    partial void OnDetailChanged(string value)
    {
        if (!_suppressOptionsChanged)
        {
            OptionsChanged?.Invoke();
        }
    }

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
        RegenerateCommand.NotifyCanExecuteChanged();
        SaveNoteCommand.NotifyCanExecuteChanged();
        SavePatientCommand.NotifyCanExecuteChanged();
        OnPropertyChanged(nameof(NoteDocumentReady));
        OnPropertyChanged(nameof(PatientDocumentReady));
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
        TranslationLanguage = "";
        TranslationRunning = false;
        EditedStamp = "";
        RegenerateWarningOpen = false;
        PatientStale = false;
    }
}
