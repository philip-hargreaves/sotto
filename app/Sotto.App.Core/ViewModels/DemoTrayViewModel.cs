using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Sotto.App.Core.Demo;

namespace Sotto.App.Core.ViewModels;

/// <summary>
/// The dev-only replay transport: bundled tracks, play/pause/stop, speed,
/// monitor audio and progress. Never visible to a clinician.
/// </summary>
public sealed partial class DemoTrayViewModel : ObservableObject
{
    private static readonly double[] Speeds = [1, 4, 8, 16];

    private readonly ConsultationViewModel _session;

    public DemoTrayViewModel(ConsultationViewModel session, IReadOnlyList<DemoTrack>? tracks = null)
    {
        _session = session;
        Tracks = new List<DemoTrack>(tracks ?? DemoTracks.Load());
        SelectedTrack = Tracks.FirstOrDefault();
        _session.PropertyChanged += (_, e) =>
        {
            switch (e.PropertyName)
            {
                case nameof(ConsultationViewModel.State):
                    OnPropertyChanged(nameof(IsReplaying));
                    PlayCommand.NotifyCanExecuteChanged();
                    StopCommand.NotifyCanExecuteChanged();
                    TogglePauseCommand.NotifyCanExecuteChanged();
                    break;
                case nameof(ConsultationViewModel.Paused):
                    OnPropertyChanged(nameof(PauseGlyph));
                    break;
                case nameof(ConsultationViewModel.AudioSeconds):
                    OnPropertyChanged(nameof(ProgressFraction));
                    OnPropertyChanged(nameof(ProgressText));
                    break;
                default:
                    break;
            }
        };
    }

    public List<DemoTrack> Tracks { get; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(TrackName))]
    [NotifyPropertyChangedFor(nameof(ProgressText))]
    [NotifyPropertyChangedFor(nameof(ProgressFraction))]
    [NotifyCanExecuteChangedFor(nameof(PlayCommand))]
    public partial DemoTrack? SelectedTrack { get; set; }

    // Read once per selection, not on every progress tick
    private double _durationSeconds;

    partial void OnSelectedTrackChanged(DemoTrack? value) =>
        _durationSeconds = value is null ? 0 : DemoTracks.DurationSeconds(value.Path);

    public string TrackName => SelectedTrack?.Name ?? "no track";

    /// <summary>A browsed file becomes a selectable track named after itself.</summary>
    public void UseTrack(string path)
    {
        var track = new DemoTrack(Path.GetFileNameWithoutExtension(path), path);
        Tracks.Add(track);
        OnPropertyChanged(nameof(Tracks));
        SelectedTrack = track;
    }

    // ---- speed: cycles 1 -> 4 -> 8 -> 16; anything over 1x is smoke-only

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(SpeedLabel))]
    [NotifyPropertyChangedFor(nameof(IsSmoke))]
    public partial double Speed { get; set; } = 1;

    public bool IsSmoke => Speed > 1;

    public string SpeedLabel => $"{Speed:0}×";

    [RelayCommand]
    private void CycleSpeed()
    {
        var i = Array.IndexOf(Speeds, Speed);
        Speed = Speeds[(i < 0 ? 0 : i + 1) % Speeds.Length];
    }

    // ---- monitor audio

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(MonitorGlyph))]
    public partial bool MonitorAudio { get; set; }

    public string MonitorGlyph => MonitorAudio ? "\uE767" : "\uE74F";  // volume / mute

    // ---- transport

    public bool IsReplaying => _session.State == SessionState.Recording
        && _session.ActiveReplay is not null;

    public string PauseGlyph => _session.Paused ? "\uE768" : "\uE769";  // play / pause

    [RelayCommand(CanExecute = nameof(CanPlay))]
    private Task Play() => _session.StartRecordingAsync(
        new ReplayRequest(SelectedTrack!.Path, Speed, MonitorAudio));

    private bool CanPlay() => _session.State == SessionState.Idle && SelectedTrack is not null;

    [RelayCommand(CanExecute = nameof(IsReplaying))]
    private Task Stop() => _session.StopRecordingAsync();

    [RelayCommand(CanExecute = nameof(IsReplaying))]
    private Task TogglePause() => _session.SetPausedAsync(!_session.Paused);

    // ---- progress, from delivered audio against the wav's own duration

    public double ProgressFraction => _durationSeconds <= 0
        ? 0
        : Math.Min(1.0, _session.AudioSeconds / _durationSeconds);

    public string ProgressText => $"{Clock(_session.AudioSeconds)} / {Clock(_durationSeconds)}";

    private static string Clock(double seconds)
    {
        var whole = (int)Math.Round(seconds);
        return $"{whole / 60}:{whole % 60:00}";
    }
}
