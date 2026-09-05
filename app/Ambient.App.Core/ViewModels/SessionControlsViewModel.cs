using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace Ambient.App.Core.ViewModels;

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
                OnPropertyChanged(nameof(RefusedVisible));
                DoneCommand.NotifyCanExecuteChanged();
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
        _session.Status.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(StatusBarViewModel.MicLevel))
            {
                OnPropertyChanged(nameof(Level));
            }
        };
    }

    /// <summary>Microphone level, 0 to 1, for the ring around the disc.</summary>
    public double Level => _session.Status.MicLevel;

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

    public bool RefusedVisible => _session.State == SessionState.Refused;

    /// <summary>The refusal card reads its reason and override from here.</summary>
    public NoteViewModel Note => _session.Note;

    // One negation of ReviewVisible, so the picker and New consultation
    // can never both appear
    public bool MicPickerVisible => !ReviewVisible;

    /// <summary>Pinned once recording: changes apply to the next consultation.</summary>
    public bool MicPickerEnabled => _session.State == SessionState.Idle;

    // The centre holds until the note streams; panes and centre never coexist
    public bool CentreStageVisible =>
        _session.State is SessionState.Idle or SessionState.Recording or SessionState.Refused
        || _session.State == SessionState.Finalising && _session.Phase != FinalisePhase.Streaming;

    public bool PanesVisible => !CentreStageVisible;

    public bool FinalisingVisible =>
        _session.State == SessionState.Finalising && _session.Phase != FinalisePhase.Streaming;

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

    [RelayCommand(CanExecute = nameof(CanDone))]
    private Task Done() => _session.CloseReviewAsync();

    private bool CanDone() => _session.State == SessionState.Refused;
}
