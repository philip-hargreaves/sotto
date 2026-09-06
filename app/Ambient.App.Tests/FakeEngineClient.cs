using System.Text.Json;
using Ambient.Client;

namespace Ambient.App.Tests;

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

    /// <summary>
    /// Runs before a request is answered: the engine pushes notifications
    /// during a blocking request (finalise stages during session/stop).
    /// </summary>
    public Action<string>? BeforeReply { get; set; }

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

        BeforeReply?.Invoke(method);

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
                new { firstUse = FirstUse, ready = ModelsCompiled, strayNoteHost = StrayNoteHost }));
        }

        if (method == "translate/languages")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(
                new { languages = FakeLanguages }));
        }

        if (method == "audio/inputs")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(new
            {
                devices = AudioInputs
                    .Select(d => new
                    {
                        id = d.Id,
                        name = d.Name,
                        shortName = d.ShortName,
                        isDefault = d.IsDefault,
                        bluetooth = d.Bluetooth,
                    })
                    .ToArray(),
            }));
        }

        if (method == "engine/models")
        {
            var models = new List<object>
            {
                // An ablation export listed first: the default tier must still win
                new { id = "whisper-turbo-int8-wordts", name = "", task = "asr", tier = "wordts", device = "GPU", licence = "MIT", active = false },
                new { id = "whisper-turbo-int8", name = "Whisper Large v3 Turbo", task = "asr", tier = "default", device = "GPU", licence = "MIT", active = true },
                new { id = "qwen3.5-9b-int4", name = "Qwen3.5 9B", task = "note", tier = "default", device = "GPU", licence = "Apache-2.0", active = NoteTier == "default" },
            };
            foreach (var (id, name, tier) in ExtraNoteModels)
            {
                models.Add(new { id, name, task = "note", tier, device = "GPU", licence = "Apache-2.0", active = NoteTier == tier });
            }

            return Task.FromResult(JsonSerializer.SerializeToElement(new { models }));
        }

        if (method == "note/tier")
        {
            var tier = JsonDocument.Parse(Requests[^1].Params).RootElement
                .GetProperty("tier").GetString() ?? "default";
            NoteTier = tier;
            return Task.FromResult(JsonSerializer.SerializeToElement(new
            {
                tier,
                id = tier == "default" ? "qwen3.5-9b-int4" : "",
                name = tier == "default" ? "Qwen3.5 9B" : "",
                state = NoteTierReply,
            }));
        }

        if (method == "engine/metrics")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(
                new { devices = new { asr = "GPU.0" }, asrRealtimeFactor = MetricsRealtimeFactor }));
        }

        if (method == "anchor/status")
        {
            return Task.FromResult(JsonSerializer.SerializeToElement(new
            {
                origin = AnchorOrigin,
                sessions = AnchorSessions,
                enrolledAt = AnchorEnrolledAt,
            }));
        }

        if (method == "anchor/clear")
        {
            AnchorOrigin = "none";
            AnchorSessions = 0;
            AnchorEnrolledAt = null;
            return Task.FromResult(Empty);
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

    /// <summary>Served by engine/metrics; healthy by default.</summary>
    public double MetricsRealtimeFactor { get; set; } = 33.4;

    public List<Ambient.App.Core.ViewModels.MicDevice> AudioInputs { get; set; } = [];

    /// <summary>Served by anchor/status; nothing learned by default.</summary>
    public string AnchorOrigin { get; set; } = "none";

    public int AnchorSessions { get; set; }

    public long? AnchorEnrolledAt { get; set; }

    /// <summary>Note models beyond the 9B that engine/models lists: (id, name, tier).</summary>
    public List<(string Id, string Name, string Tier)> ExtraNoteModels { get; } = [];

    /// <summary>The tier the engine's note lane is on; note/tier moves it.</summary>
    public string NoteTier { get; set; } = "default";

    /// <summary>The lane state note/tier answers with.</summary>
    public string NoteTierReply { get; set; } = "loading";

    /// <summary>Served by engine/readiness; warm and compiled by default.</summary>
    public bool FirstUse { get; set; }

    public bool ModelsCompiled { get; set; } = true;

    /// <summary>Served by engine/readiness; no wedged note process by default.</summary>
    public bool StrayNoteHost { get; set; }

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
