namespace Sotto.App.Core.Hosting;

public enum EngineFaultKind
{
    SessionInterrupted,
    CrashLoop,
    LaunchFailed,
}

public sealed record EngineFault(EngineFaultKind Kind, int? ExitCode = null);
