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
                or nameof(ConsultationViewModel.EngineReady))
            {
                OnPropertyChanged(nameof(State));
                OnPropertyChanged(nameof(IdleVisible));
                OnPropertyChanged(nameof(RecordingVisible));
                OnPropertyChanged(nameof(ReviewVisible));
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

    public SessionState State => _session.State;

    // The command bar swaps controls by state; computed here so it is testable
    public bool IdleVisible => _session.State == SessionState.Idle;

    public bool RecordingVisible => _session.State == SessionState.Recording;

    public bool ReviewVisible => _session.State == SessionState.Review;

    public string ElapsedLabel =>
        TimeSpan.FromSeconds(_session.AudioSeconds).ToString(@"mm\:ss",
            System.Globalization.CultureInfo.InvariantCulture);

    [RelayCommand(CanExecute = nameof(CanStartRecording))]
    private Task StartRecording() => _session.StartRecordingAsync();

    private bool CanStartRecording() =>
        _session.State == SessionState.Idle && _session.EngineReady;

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
