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
    public void SessionProgressFixtureCarriesStage()
    {
        var root = LoadFixture("session-progress.json").RootElement;

        Assert.Equal("session/progress", root.GetProperty("method").GetString());
        Assert.Equal("speakers", root.GetProperty("params").GetProperty("stage").GetString());
    }

    [Fact]
    public void SessionListFixtureCarriesTheLabelAndEditStamp()
    {
        var session = LoadFixture("session-list.json").RootElement
            .GetProperty("result").GetProperty("sessions")[0];

        Assert.Equal("finalised", session.GetProperty("state").GetString());
        Assert.False(string.IsNullOrEmpty(session.GetProperty("label").GetString()));
        Assert.Equal(JsonValueKind.Null, session.GetProperty("editedAt").ValueKind);
    }

    [Fact]
    public void SessionNoteFixtureCarriesTheRecordFields()
    {
        var note = LoadFixture("session-note.json").RootElement.GetProperty("result");

        Assert.False(string.IsNullOrEmpty(note.GetProperty("text").GetString()));
        Assert.Equal("prose", note.GetProperty("style").GetString());
        Assert.Equal("standard", note.GetProperty("detail").GetString());
        Assert.True(DateTimeOffset.TryParse(note.GetProperty("generatedAt").GetString(), out _));
        Assert.True(DateTimeOffset.TryParse(note.GetProperty("editedAt").GetString(), out _));
    }

    [Fact]
    public void SessionPatientFixtureCarriesTheTranslation()
    {
        var patient = LoadFixture("session-patient.json").RootElement.GetProperty("result");

        Assert.Equal("en", patient.GetProperty("language").GetString());
        var translation = patient.GetProperty("translation");
        Assert.Equal("pl", translation.GetProperty("language").GetString());
        Assert.Contains("łokcia", translation.GetProperty("text").GetString(), StringComparison.Ordinal);
    }

    [Fact]
    public void SessionLabelFixtureIsARequestWithIdAndText()
    {
        var root = LoadFixture("session-label.json").RootElement;

        Assert.Equal("session/label", root.GetProperty("method").GetString());
        Assert.Equal("Elbow swelling", root.GetProperty("params").GetProperty("text").GetString());
    }

    [Fact]
    public void ErrorFixtureCarriesReservedCode()
    {
        var error = LoadFixture("error-method-not-found.json").RootElement.GetProperty("error");
        Assert.Equal(-32601, error.GetProperty("code").GetInt32());
    }
}
