using System.IO.Pipes;
using System.Text;
using System.Text.Json;

namespace Sotto.Client.Tests;

public class PipeTransportTest
{
    private static string UniquePipeName() => $"LOCAL\\sotto-test-{Guid.NewGuid():N}";

    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(5);

    [Fact]
    public async Task RequestReceivesItsOwnResponse()
    {
        var name = UniquePipeName();
        await using var fake = new FakeEngine(name, request =>
        {
            var id = request.RootElement.GetProperty("id").GetInt64();
            return [$"{{\"jsonrpc\":\"2.0\",\"id\":{id},\"result\":{{\"echoedId\":{id}}}}}"];
        });
        await using var transport = await PipeTransport.ConnectAsync(name, Timeout);

        var first = await transport.RequestAsync("engine/echo", null, Timeout);
        var second = await transport.RequestAsync("engine/echo", null, Timeout);

        Assert.Equal(1, first.GetProperty("echoedId").GetInt64());
        Assert.Equal(2, second.GetProperty("echoedId").GetInt64());
    }

    [Fact]
    public async Task ResponsesCorrelateWhenDeliveredOutOfOrder()
    {
        var name = UniquePipeName();
        using var server = new NamedPipeServerStream(
            name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
        var serverTask = Task.Run(async () =>
        {
            await server.WaitForConnectionAsync();
            var ids = new List<long>();
            for (var i = 0; i < 2; i++)
            {
                var header = new byte[Framing.HeaderBytes];
                await server.ReadExactlyAsync(header);
                var body = new byte[Framing.ReadDeclaredLength(header)];
                await server.ReadExactlyAsync(body);
                using var doc = JsonDocument.Parse(body);
                ids.Add(doc.RootElement.GetProperty("id").GetInt64());
            }

            // Answer the second request first
            foreach (var id in Enumerable.Reverse(ids))
            {
                var reply = Encoding.UTF8.GetBytes(
                    $"{{\"jsonrpc\":\"2.0\",\"id\":{id},\"result\":{{\"echoedId\":{id}}}}}");
                await server.WriteAsync(Framing.Encode(reply));
            }

            await server.FlushAsync();
        });
        await using var transport = await PipeTransport.ConnectAsync(name, Timeout);

        var first = transport.RequestAsync("engine/echo", null, Timeout);
        var second = transport.RequestAsync("engine/echo", null, Timeout);
        var results = await Task.WhenAll(first, second);

        Assert.Equal(1, results[0].GetProperty("echoedId").GetInt64());
        Assert.Equal(2, results[1].GetProperty("echoedId").GetInt64());
        await serverTask;
    }

    [Fact]
    public async Task ConnectRejectsAServerWhosePidIsNotExpected()
    {
        var name = UniquePipeName();
        await using var fake = new FakeEngine(name, _ => []);
        // The fake server runs in this test process; demand a pid it cannot have.
        var wrongPid = (uint)(Environment.ProcessId + 1);

        var error = await Assert.ThrowsAsync<IOException>(
            () => PipeTransport.ConnectAsync(name, Timeout, wrongPid));
        Assert.Contains("expected engine pid", error.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task ConnectAcceptsAServerWithTheExpectedPid()
    {
        var name = UniquePipeName();
        await using var fake = new FakeEngine(name, request =>
        {
            var id = request.RootElement.GetProperty("id").GetInt64();
            return [$"{{\"jsonrpc\":\"2.0\",\"id\":{id},\"result\":{{}}}}"];
        });

        await using var transport = await PipeTransport.ConnectAsync(
            name, Timeout, (uint)Environment.ProcessId);
        var result = await transport.RequestAsync("engine/echo", null, Timeout);

        Assert.Equal(JsonValueKind.Object, result.ValueKind);
    }

    [Fact]
    public async Task ErrorResponseSurfacesAsException()
    {
        var name = UniquePipeName();
        await using var fake = new FakeEngine(name, request =>
        {
            var id = request.RootElement.GetProperty("id").GetInt64();
            return
            [
                $"{{\"jsonrpc\":\"2.0\",\"id\":{id},\"error\":{{\"code\":-32601,\"message\":\"Method not found\"}}}}",
            ];
        });
        await using var transport = await PipeTransport.ConnectAsync(name, Timeout);

        var error = await Assert.ThrowsAsync<EngineErrorException>(
            () => transport.RequestAsync("engine/nope", null, Timeout));
        Assert.Equal(-32601, error.Code);
    }

    [Fact]
    public async Task MalformedResponseFaultsItsOwnRequestNotByTimeout()
    {
        var name = UniquePipeName();
        await using var fake = new FakeEngine(name, request =>
        {
            var id = request.RootElement.GetProperty("id").GetInt64();
            // An id but neither result nor error: the request must fault, not hang.
            return [$"{{\"jsonrpc\":\"2.0\",\"id\":{id}}}"];
        });
        await using var transport = await PipeTransport.ConnectAsync(name, Timeout);

        // The missing result member, not the cancellation a timeout would raise
        await Assert.ThrowsAsync<KeyNotFoundException>(
            () => transport.RequestAsync("engine/echo", null, Timeout));
    }

    [Fact]
    public async Task TimeoutFiresWhenServerStaysSilent()
    {
        var name = UniquePipeName();
        await using var fake = new FakeEngine(name, _ => []);
        await using var transport = await PipeTransport.ConnectAsync(name, Timeout);

        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => transport.RequestAsync("engine/echo", null, TimeSpan.FromMilliseconds(200)));
    }

    [Fact]
    public async Task NotificationRaisesEventWithoutDisturbingRequests()
    {
        var name = UniquePipeName();
        await using var fake = new FakeEngine(name, request =>
        {
            var id = request.RootElement.GetProperty("id").GetInt64();
            return
            [
                "{\"jsonrpc\":\"2.0\",\"method\":\"engine/event\",\"params\":{\"n\":1}}",
                $"{{\"jsonrpc\":\"2.0\",\"id\":{id},\"result\":{{}}}}",
            ];
        });
        await using var transport = await PipeTransport.ConnectAsync(name, Timeout);
        var notified = new TaskCompletionSource<string>();
        transport.NotificationReceived += (method, _) => notified.TrySetResult(method);

        await transport.RequestAsync("engine/echo", null, Timeout);

        Assert.Equal("engine/event", await notified.Task.WaitAsync(Timeout));
    }

    [Fact]
    public async Task OversizeHeaderFaultsPendingRequests()
    {
        var name = UniquePipeName();
        using var raw = new NamedPipeServerStream(
            name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
        var serverTask = Task.Run(async () =>
        {
            await raw.WaitForConnectionAsync();
            // Read and discard the client's request, then answer with a header
            // that declares a body far past the cap
            var header = new byte[Framing.HeaderBytes];
            await raw.ReadExactlyAsync(header);
            await raw.ReadExactlyAsync(new byte[Framing.ReadDeclaredLength(header)]);
            await raw.WriteAsync(BitConverter.GetBytes(uint.MaxValue));
            await raw.FlushAsync();
        });
        await using var transport = await PipeTransport.ConnectAsync(name, Timeout);

        await Assert.ThrowsAsync<InvalidDataException>(
            () => transport.RequestAsync("engine/echo", null, Timeout));
        await serverTask;
    }

    [Fact]
    public async Task NonAsciiWiresAsReadableUtf8()
    {
        var name = UniquePipeName();
        await using var fake = new FakeEngine(name, request =>
        {
            var id = request.RootElement.GetProperty("id").GetInt64();
            return [$"{{\"jsonrpc\":\"2.0\",\"id\":{id},\"result\":{{}}}}"];
        });
        await using var transport = await PipeTransport.ConnectAsync(name, Timeout);
        const string clinical = "naïve café-au-lait 東京 µg °C";

        await transport.RequestAsync("engine/echo", new { payload = clinical }, Timeout);

        Assert.Contains(clinical, fake.LastRequestJson, StringComparison.Ordinal);
        Assert.DoesNotContain("\\u", fake.LastRequestJson, StringComparison.Ordinal);
    }
}
