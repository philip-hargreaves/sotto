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

    private static readonly string[] FakeLanguages = ["French", "Polish", "Urdu"];

    public event Action<string, JsonElement>? NotificationReceived;

    public event Action<bool>? ConnectedChanged;

    public bool Connected { get; private set; } = true;

    public void SetConnected(bool connected)
    {
        Connected = connected;
        ConnectedChanged?.Invoke(connected);
    }

    /// <summary>Every request, as (method, serialised params).</summary>
    public List<(string Method, string Params)> Requests { get; } = [];

    /// <summary>Thrown by the next matching request, once; null answers normally.</summary>
    public Func<string, Exception?>? FailNext { get; set; }

    public Task<JsonElement> RequestAsync(
        string method, object? parameters, TimeSpan timeout,
        CancellationToken cancellationToken = default)
    {
        Requests.Add((method, parameters is null ? "" : JsonSerializer.Serialize(parameters)));

        if (FailNext?.Invoke(method) is { } failure)
        {
            FailNext = null;
            return Task.FromException<JsonElement>(failure);
        }

        if (method == "engine/hello")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(
                new PeerInfo(EngineInfo.Name, EngineInfo.Version, Protocol.ProtocolVersion),
                Protocol.JsonOptions));
        }

        if (method == "session/start")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(new { sessionId = "s1" }));
        }

        if (method == "session/stop")
        {
            if (autoNotify)
            {
                _ = NotifySequenceAsync();
            }

            return Task.FromResult(JsonSerializer.SerializeToElement(new { sessionId = "s1" }));
        }

        if (method == "engine/readiness")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(
                new { firstUse = FirstUse, ready = ModelsCompiled }));
        }

        if (method == "translate/languages")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(
                new { languages = FakeLanguages }));
        }

        if (method == "engine/metrics")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(
                new { devices = new { asr = "GPU.0" }, asrRealtimeFactor = 33.4 }));
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

    /// <summary>Served by engine/readiness; warm and compiled by default.</summary>
    public bool FirstUse { get; set; }

    public bool ModelsCompiled { get; set; } = true;

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
