using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Unicode;

namespace Ambient.Client;

public sealed record PeerInfo(string Name, string Version, int ProtocolVersion);

public static class Protocol
{
    public const int ProtocolVersion = 1;

    // Non-ASCII patient and drug names must wire as readable UTF-8, not \uXXXX escapes
    public static JsonSerializerOptions JsonOptions { get; } = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        Encoder = JavaScriptEncoder.Create(UnicodeRanges.All),
    };
}

/// <summary>A JSON-RPC error response from the engine.</summary>
public sealed class EngineErrorException(int code, string message, JsonElement? data)
    : Exception(message)
{
    public int Code { get; } = code;

    public JsonElement? ErrorData { get; } = data;
}
