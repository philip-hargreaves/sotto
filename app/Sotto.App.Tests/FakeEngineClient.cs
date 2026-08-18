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

    /// <summary>Every request, as (method, serialised params).</summary>
    public List<(string Method, string Params)> Requests { get; } = [];

    public Task<JsonElement> RequestAsync(
        string method, object? parameters, TimeSpan timeout,
        CancellationToken cancellationToken = default)
    {
        Requests.Add((method, parameters is null ? "" : JsonSerializer.Serialize(parameters)));

        if (method == "engine/hello")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(
                new PeerInfo(EngineInfo.Name, EngineInfo.Version, Protocol.ProtocolVersion),
                Protocol.JsonOptions));
        }

        if (method == "session/stop")
        {
            if (autoNotify)
            {
                _ = NotifySequenceAsync();
            }

            return Task.FromResult(JsonSerializer.SerializeToElement(new { sessionId = "s1" }));
        }

        if (method == "session/transcript")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(new
            {
                turns = Transcript
                    .Select(t => new { firstFrame = 0, frameCount = 0, speaker = t.Speaker, text = t.Text })
                    .ToArray(),
            }));
        }

        return Task.FromResult(Empty);
    }

    /// <summary>Turns served by session/transcript after a stop.</summary>
    public List<(string Speaker, string Text)> Transcript { get; } = [];

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
