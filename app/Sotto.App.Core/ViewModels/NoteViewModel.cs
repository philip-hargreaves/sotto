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

    partial void OnPipelineStateChanged(NotePipelineState value) =>
        TranslateCommand.NotifyCanExecuteChanged();

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
