using System.Collections.Concurrent;
using System.IO.Pipes;
using System.Security.Principal;
using System.Text.Json;

namespace Sotto.Client;

/// <summary>
/// JSON-RPC client over the engine's named pipe. One connection, concurrent
/// requests correlated by id, notifications surfaced as an event.
/// </summary>
public sealed class PipeTransport : IAsyncDisposable
{
    private readonly NamedPipeClientStream _pipe;
    private readonly ConcurrentDictionary<long, TaskCompletionSource<JsonElement>> _pending = new();
    private readonly SemaphoreSlim _writeLock = new(1, 1);
    private readonly CancellationTokenSource _closed = new();
    private Task _readLoop = Task.CompletedTask;
    private long _nextId;

    public event Action<string, JsonElement>? NotificationReceived;

    private PipeTransport(NamedPipeClientStream pipe) => _pipe = pipe;

    public static async Task<PipeTransport> ConnectAsync(
        string pipeName, TimeSpan timeout, CancellationToken cancellationToken = default)
    {
        // Individual rights, never generics: the engine's DACL withholds the
        // instance-creation bit that generic write would demand
        // ReadPermissions backs CurrentUserOnly's server-owner check
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
        var id = Interlocked.Increment(ref _nextId);
        var completion = new TaskCompletionSource<JsonElement>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        _pending[id] = completion;
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

            await SendAsync(request, cancellationToken).ConfigureAwait(false);
            using var cts = CancellationTokenSource.CreateLinkedTokenSource(
                cancellationToken, _closed.Token);
            cts.CancelAfter(timeout);
            return await completion.Task.WaitAsync(cts.Token).ConfigureAwait(false);
        }
        finally
        {
            _pending.TryRemove(id, out _);
        }
    }

    private async Task SendAsync(object message, CancellationToken cancellationToken)
    {
        var payload = JsonSerializer.SerializeToUtf8Bytes(message, Protocol.JsonOptions);
        var frame = Framing.Encode(payload);
        await _writeLock.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            await _pipe.WriteAsync(frame, cancellationToken).ConfigureAwait(false);
            await _pipe.FlushAsync(cancellationToken).ConfigureAwait(false);
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
            while (!_closed.IsCancellationRequested)
            {
                await _pipe.ReadExactlyAsync(header, _closed.Token).ConfigureAwait(false);
                var length = Framing.ReadDeclaredLength(header);
                var body = new byte[length];
                await _pipe.ReadExactlyAsync(body, _closed.Token).ConfigureAwait(false);
                Dispatch(JsonDocument.Parse(body));
            }
        }
        catch (Exception e)
        {
            FaultAllPending(e);
        }
    }

    private void Dispatch(JsonDocument document)
    {
        var root = document.RootElement;
        if (!root.TryGetProperty("id", out var idElement))
        {
            if (root.TryGetProperty("method", out var method))
            {
                var parameters = root.TryGetProperty("params", out var p) ? p.Clone() : default;
                NotificationReceived?.Invoke(method.GetString() ?? "", parameters);
            }

            return;
        }

        if (!idElement.TryGetInt64(out var id) || !_pending.TryRemove(id, out var completion))
        {
            return;
        }

        if (root.TryGetProperty("error", out var error))
        {
            var data = error.TryGetProperty("data", out var d) ? d.Clone() : (JsonElement?)null;
            completion.TrySetException(new EngineErrorException(
                error.GetProperty("code").GetInt32(),
                error.GetProperty("message").GetString() ?? "", data));
        }
        else
        {
            completion.TrySetResult(root.GetProperty("result").Clone());
        }
    }

    private void FaultAllPending(Exception e)
    {
        foreach (var id in _pending.Keys)
        {
            if (_pending.TryRemove(id, out var completion))
            {
                completion.TrySetException(e);
            }
        }
    }

    public async ValueTask DisposeAsync()
    {
        await _closed.CancelAsync().ConfigureAwait(false);
        await _pipe.DisposeAsync().ConfigureAwait(false);
        try
        {
            await _readLoop.ConfigureAwait(false);
        }
        catch (Exception)
        {
            // The loop's failure already faulted every pending request
        }

        _closed.Dispose();
        _writeLock.Dispose();
    }
}
