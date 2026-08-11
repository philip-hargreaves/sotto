using System.Text.Json;
using Sotto.App.Core.Hosting;
using Sotto.Client;

namespace Sotto.App.Tests;

public class EngineConnectionTest
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(5);

    // The connect path is fire-and-forget, so its effects land asynchronously
    private static async Task WaitUntilAsync(Func<bool> condition)
    {
        for (var i = 0; i < 500 && !condition(); i++)
        {
            await Task.Delay(10);
        }

        Assert.True(condition());
    }

    private static readonly JsonElement Empty = JsonSerializer.SerializeToElement(new { });

    private sealed class FakeHost : IEngineHost
    {
        public event Action<EngineStatus>? StatusChanged;

        public EngineStatus Status { get; private set; } = EngineStatus.Stopped;

        public EngineFault? Fault => null;

        public int? EnginePid { get; set; } = 4321;

        public void Start()
        {
        }

        public void Shutdown()
        {
        }

        public void RaiseStatus(EngineStatus status)
        {
            Status = status;
            StatusChanged?.Invoke(status);
        }
    }

    private sealed class FakeTransport : IEngineClient
    {
        public int HelloProtocol { get; init; } = Protocol.ProtocolVersion;

        public List<string> Requests { get; } = [];

        public bool Disposed { get; private set; }

        public TaskCompletionSource<JsonElement>? PendingResponse { get; set; }

        public event Action<string, JsonElement>? NotificationReceived;

        public Task<JsonElement> RequestAsync(
            string method, object? parameters, TimeSpan timeout,
            CancellationToken cancellationToken = default)
        {
            Requests.Add(method);
            if (method == "engine/hello")
            {
                return Task.FromResult(JsonSerializer.SerializeToElement(
                    new PeerInfo(EngineInfo.Name, EngineInfo.Version, HelloProtocol),
                    Protocol.JsonOptions));
            }

            return PendingResponse?.Task ?? Task.FromResult(Empty);
        }

        public void RaiseNotification(string method) =>
            NotificationReceived?.Invoke(method, Empty);

        public ValueTask DisposeAsync()
        {
            Disposed = true;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class Rig
    {
        public FakeHost Host { get; } = new();

        public List<FakeTransport> Transports { get; } = [];

        public List<uint> ConnectPids { get; } = [];

        public int HelloProtocol { get; set; } = Protocol.ProtocolVersion;

        public Exception? ConnectFailure { get; set; }

        public TaskCompletionSource<IEngineClient>? PendingConnect { get; set; }

        public EngineConnection Connection { get; }

        public Rig()
        {
            Connection = new EngineConnection(Host, (pid, _) =>
            {
                ConnectPids.Add(pid);
                if (ConnectFailure is not null)
                {
                    throw ConnectFailure;
                }

                if (PendingConnect is { } pending)
                {
                    PendingConnect = null;
                    return pending.Task;
                }

                var transport = new FakeTransport { HelloProtocol = HelloProtocol };
                Transports.Add(transport);
                return Task.FromResult<IEngineClient>(transport);
            });
        }
    }

    [Fact]
    public async Task ConnectsWithHelloWhenTheEngineComesUp()
    {
        var rig = new Rig();

        rig.Host.RaiseStatus(EngineStatus.Running);

        Assert.Equal(4321u, Assert.Single(rig.ConnectPids));
        var transport = Assert.Single(rig.Transports);
        Assert.Equal("engine/hello", Assert.Single(transport.Requests));

        await rig.Connection.RequestAsync("session/start", null, Timeout);
        Assert.Equal("session/start", transport.Requests[^1]);
    }

    [Fact]
    public async Task RequestsFailFastWhileDisconnected()
    {
        var rig = new Rig();

        await Assert.ThrowsAsync<IOException>(
            () => rig.Connection.RequestAsync("session/start", null, Timeout));
    }

    [Fact]
    public async Task ReconnectsAfterARestart()
    {
        var rig = new Rig();
        rig.Host.RaiseStatus(EngineStatus.Running);

        rig.Host.RaiseStatus(EngineStatus.Restarting);
        Assert.True(rig.Transports[0].Disposed);
        await Assert.ThrowsAsync<IOException>(
            () => rig.Connection.RequestAsync("session/start", null, Timeout));

        rig.Host.RaiseStatus(EngineStatus.Running);
        Assert.Equal(2, rig.Transports.Count);
        await rig.Connection.RequestAsync("session/start", null, Timeout);
        Assert.Equal("session/start", rig.Transports[1].Requests[^1]);
    }

    [Fact]
    public async Task DropsTheTransportOnAFault()
    {
        var rig = new Rig();
        rig.Host.RaiseStatus(EngineStatus.Running);

        rig.Host.RaiseStatus(EngineStatus.Faulted);

        Assert.True(rig.Transports[0].Disposed);
        await Assert.ThrowsAsync<IOException>(
            () => rig.Connection.RequestAsync("session/start", null, Timeout));
    }

    [Fact]
    public async Task RefusesAProtocolMismatch()
    {
        var rig = new Rig { HelloProtocol = 99 };

        rig.Host.RaiseStatus(EngineStatus.Running);

        Assert.True(rig.Transports[0].Disposed);
        var thrown = await Assert.ThrowsAsync<IOException>(
            () => rig.Connection.RequestAsync("session/start", null, Timeout));
        Assert.Contains("protocol", thrown.InnerException!.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task ConnectFailureSurfacesOnRequests()
    {
        var rig = new Rig { ConnectFailure = new IOException("pipe never appeared") };

        rig.Host.RaiseStatus(EngineStatus.Running);

        var thrown = await Assert.ThrowsAsync<IOException>(
            () => rig.Connection.RequestAsync("session/start", null, Timeout));
        Assert.Equal("pipe never appeared", thrown.InnerException!.Message);
    }

    [Fact]
    public void NotificationsFlowThrough()
    {
        var rig = new Rig();
        string? seen = null;
        rig.Connection.NotificationReceived += (method, _) => seen = method;

        rig.Host.RaiseStatus(EngineStatus.Running);
        rig.Transports[0].RaiseNotification("note/ready");

        Assert.Equal("note/ready", seen);
    }

    [Fact]
    public async Task AStaleConnectDoesNotClobberANewerOne()
    {
        var rig = new Rig();
        var pending = new TaskCompletionSource<IEngineClient>();
        rig.PendingConnect = pending;
        rig.Host.RaiseStatus(EngineStatus.Running);

        rig.Host.RaiseStatus(EngineStatus.Restarting);
        rig.Host.RaiseStatus(EngineStatus.Running);
        var current = rig.Transports[^1];

        var stale = new FakeTransport();
        pending.SetResult(stale);

        await WaitUntilAsync(() => stale.Disposed);
        await rig.Connection.RequestAsync("session/start", null, Timeout);
        Assert.Equal("session/start", current.Requests[^1]);
        Assert.DoesNotContain("session/start", stale.Requests);
    }

    [Fact]
    public async Task MethodInFlightTracksTheOutstandingRequest()
    {
        var rig = new Rig();
        rig.Host.RaiseStatus(EngineStatus.Running);
        var transport = rig.Transports[0];
        transport.PendingResponse = new TaskCompletionSource<JsonElement>();

        var request = rig.Connection.RequestAsync("session/stop", null, Timeout);
        Assert.Equal("session/stop", rig.Connection.MethodInFlight);

        transport.PendingResponse.SetResult(Empty);
        await request;
        Assert.Null(rig.Connection.MethodInFlight);
    }
}
