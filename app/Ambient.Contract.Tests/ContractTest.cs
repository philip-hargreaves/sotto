using System.IO.Pipes;
using System.Text.Json;
using Ambient.Client;

namespace Ambient.Contract.Tests;

/// <summary>
/// The real shell client against the real engine process. This is the evidence
/// the two halves agree on the wire, not just against fixtures.
/// </summary>
[Collection("engine")]
public class ContractTest
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(5);

    [Fact]
    public async Task HelloReportsEngineNameAndVersion()
    {
        await using var engine = EngineProcess.Start();
        await using var client = await engine.ConnectAsync();

        var result = await client.RequestAsync(
            "engine/hello",
            new { name = "ambient-shell", version = "0.1.0", protocolVersion = Protocol.ProtocolVersion },
            Timeout);
        var peer = result.Deserialize<PeerInfo>(Protocol.JsonOptions);

        Assert.Equal(EngineInfo.Name, peer!.Name);
        Assert.Equal(EngineInfo.Version, peer.Version);
        Assert.Equal(Protocol.ProtocolVersion, peer.ProtocolVersion);
    }

    [Fact]
    public async Task EchoReturnsNonAsciiPayloadIntact()
    {
        await using var engine = EngineProcess.Start();
        await using var client = await engine.ConnectAsync();
        const string clinical = "naïve café-au-lait 東京 µg °C phénoxyméthylpénicilline";

        var result = await client.RequestAsync("engine/echo", new { payload = clinical }, Timeout);

        Assert.Equal(clinical, result.GetProperty("payload").GetString());
    }

    [Fact]
    public async Task UnknownMethodReturnsMethodNotFoundQuickly()
    {
        await using var engine = EngineProcess.Start();
        await using var client = await engine.ConnectAsync();

        var start = System.Diagnostics.Stopwatch.StartNew();
        var error = await Assert.ThrowsAsync<EngineErrorException>(
            () => client.RequestAsync("engine/nonexistent", null, Timeout));
        start.Stop();

        Assert.Equal(-32601, error.Code);
        Assert.True(start.Elapsed < TimeSpan.FromSeconds(1), $"took {start.ElapsedMilliseconds} ms");
    }

    [Fact]
    public async Task EngineDropsAMalformedFrameAndKeepsServing()
    {
        await using var engine = EngineProcess.Start();
        using var raw = new NamedPipeClientStream(
            ".", "LOCAL\\ambient-engine", PipeAccessRights.ReadData | PipeAccessRights.WriteData
                | PipeAccessRights.ReadAttributes | PipeAccessRights.WriteAttributes
                | PipeAccessRights.ReadPermissions,
            PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly,
            System.Security.Principal.TokenImpersonationLevel.Identification,
            HandleInheritability.None);
        await raw.ConnectAsync((int)Timeout.TotalMilliseconds);

        // A well-framed body that is not JSON: dropped, not fatal
        var junk = "not json at all"u8.ToArray();
        await raw.WriteAsync(BitConverter.GetBytes((uint)junk.Length));
        await raw.WriteAsync(junk);

        // A valid request on the same connection must still be answered
        var request =
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"engine/echo\",\"params\":{\"payload\":\"alive\"}}"u8
                .ToArray();
        await raw.WriteAsync(BitConverter.GetBytes((uint)request.Length));
        await raw.WriteAsync(request);
        await raw.FlushAsync();

        var header = new byte[Framing.HeaderBytes];
        await raw.ReadExactlyAsync(header);
        var body = new byte[Framing.ReadDeclaredLength(header)];
        await raw.ReadExactlyAsync(body);
        using var response = JsonDocument.Parse(body);

        Assert.Equal(
            "alive",
            response.RootElement.GetProperty("result").GetProperty("payload").GetString());
        Assert.True(engine.IsRunning);
    }

    [Fact]
    public async Task SecondEngineInstanceIsRefusedForClaimingThePipe()
    {
        await using var first = EngineProcess.Start();
        await using var client = await first.ConnectAsync();  // ensure the pipe is up

        await using var second = EngineProcess.Start();
        var exitCode = await second.WaitForExitAsync(Timeout);

        // Exit code 1 alone is shared by any startup failure; the message proves
        // it was the first-instance guard, not an unrelated crash.
        Assert.Equal(1, exitCode);
        Assert.Contains("pipe name already claimed", second.StandardError, StringComparison.Ordinal);
    }
}

[CollectionDefinition("engine", DisableParallelization = true)]
public class EngineTestGroup;
