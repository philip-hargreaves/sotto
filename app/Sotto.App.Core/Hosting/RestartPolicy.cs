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
        if (consultationActive)
        {
            return RecoveryAction.Surface;
        }

        var recent = crashes.Count(crash => now - crash <= StormWindow);
        return recent >= StormLimit ? RecoveryAction.GiveUp : RecoveryAction.Restart;
    }
}
