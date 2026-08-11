namespace Sotto.App.Core.Hosting;

/// <summary>
/// What is recorded about one engine death: metadata only, never process
/// memory. MethodInFlight is filled in once the transport is wired up.
/// </summary>
public sealed record CrashReport(
    DateTimeOffset Timestamp,
    int ExitCode,
    TimeSpan Uptime,
    int CrashCount,
    RecoveryAction Action,
    string? MethodInFlight = null);
