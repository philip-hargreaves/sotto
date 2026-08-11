using System.Text.Json;

namespace Sotto.Client;

/// <summary>
/// The shell's port to the engine. PipeTransport is the production
/// implementation; tests and shell development use a fake.
/// </summary>
public interface IEngineClient : IAsyncDisposable
{
    event Action<string, JsonElement>? NotificationReceived;

    Task<JsonElement> RequestAsync(
        string method, object? parameters, TimeSpan timeout,
        CancellationToken cancellationToken = default);
}
