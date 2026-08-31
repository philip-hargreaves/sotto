using System.Text.Json;
using Ambient.Client;

namespace Ambient.App.Core.Hosting;

/// <summary>
/// IEngineClient that follows the supervisor: connects when the engine comes
/// up, drops the transport when it goes down, and fails requests fast in
/// between. Each engine process gets its own pid-verified connection.
/// </summary>
public sealed class EngineConnection : IEngineClient
{
    private const string ShellName = "ambient-shell";
    private const string ShellVersion = "0.1.0";

    // Generous: the engine binds its pipe before model verification and
    // compilation, and answers the buffered hello only once that finishes
    private static readonly TimeSpan HelloTimeout = TimeSpan.FromSeconds(120);

    private static readonly TimeSpan RedialDelay = TimeSpan.FromMilliseconds(500);

    private readonly IEngineHost _host;
    private readonly Func<uint, CancellationToken, Task<IEngineClient>> _connect;
    private readonly CancellationTokenSource _disposal = new();
    private readonly object _gate = new();
    private IEngineClient? _transport;
    private Exception? _lastConnectError;
    private int _generation;
    private volatile string? _methodInFlight;
    private int _disposed;

    public event Action<string, JsonElement>? NotificationReceived;

    public event Action<bool>? ConnectedChanged;

    public bool Connected
    {
        get
        {
            lock (_gate)
            {
                return _transport is not null;
            }
        }
    }

    // The last request started, for the crash report; not an accounting system
    public string? MethodInFlight => _methodInFlight;

    public EngineConnection(
        IEngineHost host, Func<uint, CancellationToken, Task<IEngineClient>> connect)
    {
        _host = host;
        _connect = connect;
        host.StatusChanged += OnEngineStatusChanged;
        if (host.Status == EngineStatus.Running)
        {
            OnEngineStatusChanged(EngineStatus.Running);
        }
    }

    public async Task<JsonElement> RequestAsync(
        string method, object? parameters, TimeSpan timeout,
        CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        IEngineClient transport;
        lock (_gate)
        {
            transport = _transport
                ?? throw new IOException("engine is not connected", _lastConnectError);
        }

        _methodInFlight = method;
        try
        {
            return await transport.RequestAsync(method, parameters, timeout, cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            _methodInFlight = null;
        }
    }

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
        {
            return;
        }

        _host.StatusChanged -= OnEngineStatusChanged;
        await _disposal.CancelAsync().ConfigureAwait(false);
        IEngineClient? transport;
        lock (_gate)
        {
            transport = _transport;
            _transport = null;
        }

        if (transport is not null)
        {
            await transport.DisposeAsync().ConfigureAwait(false);
        }

        _disposal.Dispose();
    }

    private void OnEngineStatusChanged(EngineStatus status)
    {
        if (status == EngineStatus.Running)
        {
            int generation;
            lock (_gate)
            {
                generation = ++_generation;
            }

            _ = ConnectAsync(generation);
        }
        else
        {
            DropTransport();
        }
    }

    private void DropTransport()
    {
        IEngineClient? old;
        lock (_gate)
        {
            _generation++;
            old = _transport;
            _transport = null;
        }

        if (old is not null)
        {
            _ = old.DisposeAsync().AsTask();
            ConnectedChanged?.Invoke(false);
        }
    }

    private async Task ConnectAsync(int generation)
    {
        // Redial until installed or superseded; a silent give-up here was
        // once a permanently dead connection
        while (Volatile.Read(ref _disposed) == 0 && !_disposal.IsCancellationRequested)
        {
            lock (_gate)
            {
                if (_generation != generation)
                {
                    return;
                }
            }

            try
            {
                if (await TryConnectOnceAsync(generation).ConfigureAwait(false))
                {
                    return;
                }
            }
            catch (OperationCanceledException) when (_disposal.IsCancellationRequested)
            {
                return;
            }
            catch (Exception e)
            {
                lock (_gate)
                {
                    if (_generation == generation)
                    {
                        _lastConnectError = e;
                    }
                }
            }

            try
            {
                await Task.Delay(RedialDelay, _disposal.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                return;
            }
        }
    }

    // True to stop dialling: installed, or a newer generation owns it
    private async Task<bool> TryConnectOnceAsync(int generation)
    {
        var pid = _host.EnginePid ?? throw new IOException("engine pid unavailable");
        var transport = await _connect((uint)pid, _disposal.Token).ConfigureAwait(false);
        var installed = false;
        try
        {
            var hello = await transport.RequestAsync(
                "engine/hello", new PeerInfo(ShellName, ShellVersion, Protocol.ProtocolVersion),
                HelloTimeout, _disposal.Token).ConfigureAwait(false);
            var engineProtocol = hello.GetProperty("protocolVersion").GetInt32();
            if (engineProtocol != Protocol.ProtocolVersion)
            {
                throw new IOException(FormattableString.Invariant(
                    $"engine speaks protocol {engineProtocol}, this shell needs {Protocol.ProtocolVersion}"));
            }

            transport.NotificationReceived += OnInnerNotification;
            lock (_gate)
            {
                // A newer status event owns the connection now; stand down
                if (_generation == generation && _disposed == 0)
                {
                    _transport = transport;
                    _lastConnectError = null;
                    installed = true;
                }
            }

            if (installed)
            {
                ConnectedChanged?.Invoke(true);
            }

            return true;
        }
        finally
        {
            if (!installed)
            {
                await transport.DisposeAsync().ConfigureAwait(false);
            }
        }
    }

    private void OnInnerNotification(string method, JsonElement parameters) =>
        NotificationReceived?.Invoke(method, parameters);
}
