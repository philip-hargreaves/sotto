namespace Ambient.App.Core.Hosting;

public enum RecoveryAction
{
    Restart,
    Surface,
    GiveUp,
}

/// <summary>
/// The decision on an engine death: restart, bounded by a crash-storm cutoff.
/// Mid-consultation the shell resumes the stored session on the new engine,
/// so a restart saves the consult where surfacing the death used to abandon it.
/// </summary>
public static class RestartPolicy
{
    // VS Code's language-client cutoff, the reference figure
    public const int StormLimit = 5;

    public static readonly TimeSpan StormWindow = TimeSpan.FromMinutes(3);

    public static RecoveryAction Decide(
        bool consultationActive, IReadOnlyList<DateTimeOffset> crashes, DateTimeOffset now)
    {
        // A crash mid-consultation restarts and the shell resumes the session;
        // the storm limit still wins
        var recent = crashes.Count(crash => now - crash <= StormWindow);
        return recent >= StormLimit ? RecoveryAction.GiveUp : RecoveryAction.Restart;
    }
}
