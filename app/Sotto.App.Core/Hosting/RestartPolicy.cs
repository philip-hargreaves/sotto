namespace Sotto.App.Core.Hosting;

public enum RecoveryAction
{
    Restart,
    Surface,
    GiveUp,
}

/// <summary>
/// The decision on an engine death. Mid-consultation a silent restart would
/// hide a possible transcript gap, so the death is surfaced instead. While
/// idle the engine restarts silently, bounded by a crash-storm cutoff.
/// </summary>
public static class RestartPolicy
{
    // VS Code's language-client cutoff, the reference figure
    public const int StormLimit = 5;

    public static readonly TimeSpan StormWindow = TimeSpan.FromMinutes(3);

    public static RecoveryAction Decide(
        bool consultationActive, IReadOnlyList<DateTimeOffset> crashes, DateTimeOffset now)
    {
        // A crash mid-consultation restarts like any other: the audio is
        // stored as it captures and the shell resumes the session, so a
        // restart saves the consult where surfacing used to abandon it.
        // The storm limit still wins - resume cannot fix a crash loop.
        var recent = crashes.Count(crash => now - crash <= StormWindow);
        return recent >= StormLimit ? RecoveryAction.GiveUp : RecoveryAction.Restart;
    }
}
