using System.Text.Json;
using Sotto.Client;

namespace Sotto.App.Core;

/// <summary>
/// Stand-in engine until supervision wires the real transport. Answers hello
/// with the values the real engine would send, so bindings and tests exercise
/// the same shapes.
/// </summary>
public sealed class FakeEngineClient : IEngineClient
{
    public event Action<string, JsonElement>? NotificationReceived;

    public Task<JsonElement> RequestAsync(
        string method, object? parameters, TimeSpan timeout,
        CancellationToken cancellationToken = default)
    {
        var result = method switch
        {
            "engine/hello" => JsonSerializer.SerializeToElement(
                new PeerInfo(EngineInfo.Name, EngineInfo.Version, Protocol.ProtocolVersion),
                Protocol.JsonOptions),
            _ => JsonSerializer.SerializeToElement(new { }),
        };
        return Task.FromResult(result);
    }

    public void RaiseNotification(string method, JsonElement parameters) =>
        NotificationReceived?.Invoke(method, parameters);

    public ValueTask DisposeAsync() => ValueTask.CompletedTask;
}
