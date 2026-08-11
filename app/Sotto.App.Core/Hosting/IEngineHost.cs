namespace Sotto.App.Core.Hosting;

/// <summary>
/// The shell's port to the engine process lifecycle. EngineSupervisor is the
/// production implementation. StatusChanged may fire on background threads.
/// </summary>
public interface IEngineHost
{
    event Action<EngineStatus>? StatusChanged;

    EngineStatus Status { get; }

    EngineFault? Fault { get; }

    void Start();

    void Shutdown();
}
