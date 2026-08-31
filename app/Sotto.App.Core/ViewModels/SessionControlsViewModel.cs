using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace Sotto.App.Core.ViewModels;

public sealed partial class SessionControlsViewModel : ObservableObject
{
    private readonly ConsultationViewModel _session;

    public SessionControlsViewModel(ConsultationViewModel session)
    {
        _session = session;
        _session.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(ConsultationViewModel.State)
                or nameof(ConsultationViewModel.EngineReady)
                or nameof(ConsultationViewModel.Phase)
                or nameof(ConsultationViewModel.ModelsReady))
            {
                OnPropertyChanged(nameof(State));
                OnPropertyChanged(nameof(IdleVisible));
                OnPropertyChanged(nameof(RecordingVisible));
                OnPropertyChanged(nameof(ReviewVisible));
                OnPropertyChanged(nameof(MicPickerVisible));
                OnPropertyChanged(nameof(MicPickerEnabled));
                OnPropertyChanged(nameof(CentreStageVisible));
                OnPropertyChanged(nameof(PanesVisible));
                OnPropertyChanged(nameof(FinalisingVisible));
                OnPropertyChanged(nameof(FinalisingLabel));
                StartRecordingCommand.NotifyCanExecuteChanged();
                StopRecordingCommand.NotifyCanExecuteChanged();
                CancelRecordingCommand.NotifyCanExecuteChanged();
                NewConsultationCommand.NotifyCanExecuteChanged();
            }
            else if (e.PropertyName is nameof(ConsultationViewModel.AudioSeconds))
            {
                OnPropertyChanged(nameof(ElapsedLabel));
            }
        };
    }

    /// <summary>The centre-stage caption for the current finalise phase.</summary>
    public string FinalisingLabel => _session.Phase switch
    {
        FinalisePhase.Transcript => "Writing transcript",
        FinalisePhase.Speakers => "Labelling speakers",
        FinalisePhase.Turns => "Writing transcript",
        FinalisePhase.Note => "Preparing note",
        _ => "Finalising",
    };

    public SessionState State => _session.State;

    // The view swaps by state; computed here so it is testable
    public bool IdleVisible => _session.State == SessionState.Idle;

    public bool RecordingVisible => _session.State == SessionState.Recording;

    public bool ReviewVisible => _session.State == SessionState.Review;

    // The mic picker time-shares the header cell with New consultation: one
    // negation of the SAME expression, so the two can never both appear
    public bool MicPickerVisible => !ReviewVisible;

    /// <summary>Pinned once recording: changes apply to the next consultation.</summary>
    public bool MicPickerEnabled => _session.State == SessionState.Idle;

    // The centre holds until the note streams: an empty document waiting on
    // the prefill is worse than a spinner that says "Preparing note". The
    // panes and the centre never coexist.
    public bool CentreStageVisible =>
        _session.State is SessionState.Idle or SessionState.Recording
        || _session.State == SessionState.Finalising && _session.Phase != FinalisePhase.Streaming;

    public bool PanesVisible => !CentreStageVisible;

    public bool FinalisingVisible =>
        _session.State == SessionState.Finalising && _session.Phase != FinalisePhase.Streaming;

    /// <summary>The status bar owns the level data; the centre stage shows it.</summary>
    public System.Collections.ObjectModel.ObservableCollection<LevelBar> Meter =>
        _session.Status.Meter;

    public string ElapsedLabel =>
        TimeSpan.FromSeconds(_session.AudioSeconds).ToString(@"mm\:ss",
            System.Globalization.CultureInfo.InvariantCulture);

    [RelayCommand(CanExecute = nameof(CanStartRecording))]
    private Task StartRecording() => _session.StartRecordingAsync();

    private bool CanStartRecording() =>
        _session.State == SessionState.Idle && _session.EngineReady && _session.ModelsReady;

    [RelayCommand(CanExecute = nameof(CanStopRecording))]
    private Task StopRecording() => _session.StopRecordingAsync();

    private bool CanStopRecording() => _session.State == SessionState.Recording;

    [RelayCommand(CanExecute = nameof(CanCancelRecording))]
    private Task CancelRecording() => _session.CancelRecordingAsync();

    private bool CanCancelRecording() => _session.State == SessionState.Recording;

    [RelayCommand(CanExecute = nameof(CanNewConsultation))]
    private void NewConsultation() => _session.StartNewConsultation();

    private bool CanNewConsultation() => _session.State == SessionState.Review;
}
