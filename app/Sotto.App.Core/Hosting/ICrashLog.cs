namespace Sotto.App.Core.Hosting;

/// <summary>
/// Where crash reports go. FileCrashLog is the production implementation.
/// </summary>
public interface ICrashLog
{
    void Record(CrashReport report);
}
