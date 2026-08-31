using System.Text.Json;

namespace Ambient.Client;

/// <summary>
/// The shell's port to the engine. PipeTransport is the production
/// implementation; tests and shell development use a fake.
/// </summary>
public interface IEngineClient : IAsyncDisposable
{
    event Action<string, JsonElement>? NotificationReceived;

    /// <summary>True while a verified transport is up; requests can succeed.</summary>
    bool Connected { get; }

    event Action<bool>? ConnectedChanged;

    Task<JsonElement> RequestAsync(
        string method, object? parameters, TimeSpan timeout,
        CancellationToken cancellationToken = default);
}
