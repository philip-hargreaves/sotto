using System.IO.Pipes;
using System.Text;
using System.Text.Json;

namespace Sotto.Client.Tests;

public class PipeTransportTest
{
    private static string UniquePipeName() => $"LOCAL\\sotto-test-{Guid.NewGuid():N}";

    private static readonly TimeSpan _timeout = TimeSpan.FromSeconds(5);

    [Fact]
    public async Task RequestReceivesItsOwnResponse()
    {
        var name = UniquePipeName();
        await using var fake = new FakeEngine(name, request =>
        {
            var id = request.RootElement.GetProperty("id").GetInt64();
            return [$"{{\"jsonrpc\":\"2.0\",\"id\":{id},\"result\":{{\"echoedId\":{id}}}}}"];
        });
        await using var transport = await PipeTransport.ConnectAsync(name, _timeout);

        var first = await transport.RequestAsync("engine/echo", null, _timeout);
        var second = await transport.RequestAsync("engine/echo", null, _timeout);

        Assert.Equal(1, first.GetProperty("echoedId").GetInt64());
        Assert.Equal(2, second.GetProperty("echoedId").GetInt64());
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
        await using var transport = await PipeTransport.ConnectAsync(name, _timeout);

        var error = await Assert.ThrowsAsync<EngineErrorException>(
            () => transport.RequestAsync("engine/nope", null, _timeout));
        Assert.Equal(-32601, error.Code);
    }

    [Fact]
    public async Task TimeoutFiresWhenServerStaysSilent()
    {
        var name = UniquePipeName();
        await using var fake = new FakeEngine(name, _ => []);
        await using var transport = await PipeTransport.ConnectAsync(name, _timeout);

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
        await using var transport = await PipeTransport.ConnectAsync(name, _timeout);
        var notified = new TaskCompletionSource<string>();
        transport.NotificationReceived += (method, _) => notified.TrySetResult(method);

        await transport.RequestAsync("engine/echo", null, _timeout);

        Assert.Equal("engine/event", await notified.Task.WaitAsync(_timeout));
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
        await using var transport = await PipeTransport.ConnectAsync(name, _timeout);

        await Assert.ThrowsAsync<InvalidDataException>(
            () => transport.RequestAsync("engine/echo", null, _timeout));
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
        await using var transport = await PipeTransport.ConnectAsync(name, _timeout);
        const string clinical = "naïve café-au-lait 東京 µg °C";

        await transport.RequestAsync("engine/echo", new { payload = clinical }, _timeout);

        Assert.Contains(clinical, fake.LastRequestJson, StringComparison.Ordinal);
        Assert.DoesNotContain("\\u", fake.LastRequestJson, StringComparison.Ordinal);
    }
}
