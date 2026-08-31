namespace Ambient.App.Core.Hosting;

/// <summary>
/// Starts one engine process, already bound so it cannot outlive the app.
/// A seam so recovery is testable without real processes.
/// </summary>
public interface IEngineLauncher
{
    IEngineProcess Launch();
}
