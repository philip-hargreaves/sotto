using System.Text.Json;
using Sotto.Client;

namespace Sotto.App.Tests;

/// <summary>
/// Test-double engine. After session/stop it pushes note/ready then
/// patient/ready, like the real pipeline.
/// </summary>
public sealed class FakeEngineClient(bool autoNotify = true) : IEngineClient
{
    private static readonly JsonElement Empty = JsonSerializer.SerializeToElement(new { });

    public event Action<string, JsonElement>? NotificationReceived;

    public Task<JsonElement> RequestAsync(
        string method, object? parameters, TimeSpan timeout,
        CancellationToken cancellationToken = default)
    {
        if (method == "engine/hello")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(
                new PeerInfo(EngineInfo.Name, EngineInfo.Version, Protocol.ProtocolVersion),
                Protocol.JsonOptions));
        }

        if (method == "session/stop" && autoNotify)
        {
            _ = NotifySequenceAsync();
        }

        return Task.FromResult(Empty);
    }

    public void RaiseNotification(string method, JsonElement parameters = default) =>
        NotificationReceived?.Invoke(method, parameters);

    private async Task NotifySequenceAsync()
    {
        await Task.Delay(700).ConfigureAwait(false);
        RaiseNotification("note/ready");
        await Task.Delay(1500).ConfigureAwait(false);
        RaiseNotification("patient/ready");
    }

    public ValueTask DisposeAsync() => ValueTask.CompletedTask;
}
