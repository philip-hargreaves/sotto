using System.Text.Json;

namespace Sotto.Client.Tests;

/// <summary>The fixtures both languages must agree on.</summary>
public class FixtureTest
{
    private static JsonDocument LoadFixture(string name)
    {
        var dir = AppContext.BaseDirectory;
        while (dir is not null && !Directory.Exists(Path.Combine(dir, "schema", "fixtures")))
        {
            dir = Path.GetDirectoryName(dir);
        }

        Assert.NotNull(dir);
        return JsonDocument.Parse(
            File.ReadAllText(Path.Combine(dir, "schema", "fixtures", name)));
    }

    [Fact]
    public void PeerInfoSerializesToHelloRequestParams()
    {
        var serialized = JsonSerializer.SerializeToElement(
            new PeerInfo("sotto-shell", "0.1.0", Protocol.ProtocolVersion),
            Protocol.JsonOptions);
        var expected = LoadFixture("hello-request.json").RootElement.GetProperty("params");

        Assert.True(JsonElement.DeepEquals(serialized, expected));
    }

    [Fact]
    public void HelloResponseFixtureDeserializesToPeerInfo()
    {
        var result = LoadFixture("hello-response.json").RootElement.GetProperty("result");
        var peer = result.Deserialize<PeerInfo>(Protocol.JsonOptions);

        // Against the constants, so drift from the shared fixture fails here
        Assert.Equal(
            new PeerInfo(EngineInfo.Name, EngineInfo.Version, Protocol.ProtocolVersion), peer);
    }

    [Fact]
    public void EchoFixturePayloadSurvivesRoundTrip()
    {
        var fixture = LoadFixture("echo-request-nonascii.json");
        var payload = fixture.RootElement.GetProperty("params").GetProperty("payload").GetString();
        Assert.NotNull(payload);

        var reserialized = JsonSerializer.Serialize(new { payload }, Protocol.JsonOptions);
        Assert.Contains("naïve", reserialized, StringComparison.Ordinal);
        Assert.Contains("東京", reserialized, StringComparison.Ordinal);
    }

    [Fact]
    public void AudioLevelFixtureCarriesLevelAndClip()
    {
        var root = LoadFixture("audio-level.json").RootElement;

        Assert.Equal("audio.level", root.GetProperty("method").GetString());
        Assert.Equal(0.5, root.GetProperty("params").GetProperty("level").GetDouble());
        Assert.False(root.GetProperty("params").GetProperty("clipped").GetBoolean());
    }

    [Fact]
    public void SessionInterruptedFixtureCarriesReasonAndDetail()
    {
        var root = LoadFixture("session-interrupted.json").RootElement;

        Assert.Equal("session/interrupted", root.GetProperty("method").GetString());
        Assert.Equal("deviceLost", root.GetProperty("params").GetProperty("reason").GetString());
        Assert.False(
            string.IsNullOrEmpty(root.GetProperty("params").GetProperty("detail").GetString()));
    }

    [Fact]
    public void ErrorFixtureCarriesReservedCode()
    {
        var error = LoadFixture("error-method-not-found.json").RootElement.GetProperty("error");
        Assert.Equal(-32601, error.GetProperty("code").GetInt32());
    }
}
