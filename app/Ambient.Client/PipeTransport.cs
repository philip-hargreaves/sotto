using System.Collections.Concurrent;
using System.IO.Pipes;
using System.Security.Principal;
using System.Text.Json;

namespace Sotto.Client;

/// <summary>
/// JSON-RPC client over the engine's named pipe. One connection, concurrent
/// requests correlated by id, notifications surfaced as an event. Any transport
/// failure is terminal: pending and future requests observe it, and the
/// connection does not recover.
/// </summary>
public sealed class PipeTransport : IEngineClient
{
    private readonly NamedPipeClientStream _pipe;
    private readonly ConcurrentDictionary<long, TaskCompletionSource<JsonElement>> _pending = new();
    private readonly SemaphoreSlim _writeLock = new(1, 1);
    private readonly CancellationTokenSource _closed = new();
    private Task _readLoop = Task.CompletedTask;
    private long _nextId;
    private Exception? _fault;
    private int _disposed;

    public event Action<string, JsonElement>? NotificationReceived;

    // A raw transport is connected until it faults; EngineConnection is the
    // layer that raises transitions
    public bool Connected => Volatile.Read(ref _fault) is null && Volatile.Read(ref _disposed) == 0;

    public event Action<bool>? ConnectedChanged
    {
        add { }
        remove { }
    }

    private PipeTransport(NamedPipeClientStream pipe) => _pipe = pipe;

    public static async Task<PipeTransport> ConnectAsync(
        string pipeName, TimeSpan timeout, uint? expectedServerProcessId = null,
        CancellationToken cancellationToken = default)
    {
        const PipeAccessRights rights = PipeAccessRights.ReadData | PipeAccessRights.WriteData
            | PipeAccessRights.ReadAttributes | PipeAccessRights.WriteAttributes
            | PipeAccessRights.ReadPermissions;
        var pipe = new NamedPipeClientStream(
            ".", pipeName, rights,
            PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly,
            TokenImpersonationLevel.Identification, HandleInheritability.None);
        try
        {
            using var cts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            cts.CancelAfter(timeout);
            await pipe.ConnectAsync(cts.Token).ConfigureAwait(false);

            // The caller that spawned the engine knows its pid; refuse any other
            // process that may have claimed the pipe name first.
            if (expectedServerProcessId is uint expected)
            {
                var actual = ServerVerifier.GetServerProcessId(pipe);
                if (actual != expected)
                {
                    var actualText = actual?.ToString(System.Globalization.CultureInfo.InvariantCulture)
                        ?? "<unavailable>";
                    throw new IOException(FormattableString.Invariant(
                        $"pipe server pid {actualText} is not the expected engine pid {expected}"));
                }
            }
        }
        catch
        {
            await pipe.DisposeAsync().ConfigureAwait(false);
            throw;
        }

        var transport = new PipeTransport(pipe);
        transport._readLoop = transport.ReadLoopAsync();
        return transport;
    }

    public async Task<JsonElement> RequestAsync(
        string method, object? parameters, TimeSpan timeout,
        CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        if (Volatile.Read(ref _fault) is { } fault)
        {
            throw new IOException("pipe transport is closed", fault);
        }

        var id = Interlocked.Increment(ref _nextId);
        var completion = new TaskCompletionSource<JsonElement>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        _pending[id] = completion;

        // Close the register-after-fault window: a fault between the check above
        // and this insert would not have seen our completion.
        if (Volatile.Read(ref _fault) is { } raced)
        {
            _pending.TryRemove(id, out _);
            throw new IOException("pipe transport is closed", raced);
        }

        using var cts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        cts.CancelAfter(timeout);
        try
        {
            var request = new Dictionary<string, object?>
            {
                ["jsonrpc"] = "2.0",
                ["id"] = id,
                ["method"] = method,
            };
            if (parameters is not null)
            {
                request["params"] = parameters;
            }

            await SendAsync(request, cts.Token).ConfigureAwait(false);
            return await completion.Task.WaitAsync(cts.Token).ConfigureAwait(false);
        }
        finally
        {
            _pending.TryRemove(id, out _);
        }
    }

    private async Task SendAsync(object message, CancellationToken cancellationToken)
    {
        var frame = Framing.Encode(JsonSerializer.SerializeToUtf8Bytes(message, Protocol.JsonOptions));
        // One token governs the send; a cancelled write desyncs the stream, so
        // it is terminal
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken, _closed.Token);
        await _writeLock.WaitAsync(linked.Token).ConfigureAwait(false);
        try
        {
            await _pipe.WriteAsync(frame, linked.Token).ConfigureAwait(false);
            await _pipe.FlushAsync(linked.Token).ConfigureAwait(false);
        }
        catch (Exception e)
        {
            Fault(e);
            // If closure cancelled this write, surface the closure cause rather
            // than the incidental cancellation.
            if (Volatile.Read(ref _fault) is { } cause && !ReferenceEquals(cause, e))
            {
                throw new IOException("pipe transport is closed", cause);
            }

            throw;
        }
        finally
        {
            _writeLock.Release();
        }
    }

    private async Task ReadLoopAsync()
    {
        var header = new byte[Framing.HeaderBytes];
        try
        {
            while (true)
            {
                await _pipe.ReadExactlyAsync(header, _closed.Token).ConfigureAwait(false);
                var body = new byte[Framing.ReadDeclaredLength(header)];
                await _pipe.ReadExactlyAsync(body, _closed.Token).ConfigureAwait(false);
                using var document = JsonDocument.Parse(body);
                Dispatch(document.RootElement);
            }
        }
        catch (Exception e)
        {
            Fault(e);
        }
    }

    private void Dispatch(JsonElement root)
    {
        if (!root.TryGetProperty("id", out var idElement))
        {
            if (root.TryGetProperty("method", out var method))
            {
                var parameters = root.TryGetProperty("params", out var p) ? p.Clone() : default;
                try
                {
                    NotificationReceived?.Invoke(method.GetString() ?? "", parameters);
                }
                catch
                {
                    // A subscriber's failure is not a transport failure
                }
            }

            return;
        }

        if (!idElement.TryGetInt64(out var id) || !_pending.TryRemove(id, out var completion))
        {
            return;
        }

        // Build the outcome before completing: a malformed response must fault its
        // own request, never leave the removed completion stranded.
        try
        {
            if (root.TryGetProperty("error", out var error))
            {
                var data = error.TryGetProperty("data", out var d) ? d.Clone() : (JsonElement?)null;
                completion.SetException(new EngineErrorException(
                    error.GetProperty("code").GetInt32(),
                    error.GetProperty("message").GetString() ?? "", data));
            }
            else
            {
                completion.SetResult(root.GetProperty("result").Clone());
            }
        }
        catch (Exception e)
        {
            completion.TrySetException(e);
        }
    }

    private void Fault(Exception cause)
    {
        // First fault wins; the transport is terminal thereafter.
        if (Interlocked.CompareExchange(ref _fault, cause, null) is not null)
        {
            return;
        }

        // Fault the pending requests before cancelling I/O, so each observes the
        // real cause rather than a bare cancellation.
        foreach (var id in _pending.Keys)
        {
            if (_pending.TryRemove(id, out var completion))
            {
                completion.TrySetException(cause);
            }
        }

        _closed.Cancel();
    }

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
        {
            return;
        }

        Fault(new ObjectDisposedException(nameof(PipeTransport)));
        await _pipe.DisposeAsync().ConfigureAwait(false);
        try
        {
            await _readLoop.ConfigureAwait(false);
        }
        catch
        {
            // The loop's exit already faulted every pending request
        }

        // Left undisposed on purpose: parked requests must see the clean close,
        // and neither holds an unmanaged handle
    }
}
