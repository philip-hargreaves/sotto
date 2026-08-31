namespace Ambient.App.Core.Hosting;

/// <summary>
/// A launched engine as the supervisor sees it. Exited may fire on a
/// thread-pool thread; ExitCode is only meaningful once HasExited is true.
/// </summary>
public interface IEngineProcess : IDisposable
{
    event Action? Exited;

    int Id { get; }

    bool HasExited { get; }

    int ExitCode { get; }

    void Kill();
}
