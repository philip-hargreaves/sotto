using Microsoft.Win32.SafeHandles;
using Windows.Win32;
using Windows.Win32.Foundation;

namespace Ambient.App.Core.Hosting;

/// <summary>
/// Watches a launched engine through the process handle CreateProcess
/// returned, so there is no pid-reuse race and no System.Diagnostics.
/// </summary>
internal sealed class EngineProcess : IEngineProcess
{
    private readonly object _gate = new();
    private readonly SafeProcessHandle _handle;
    private readonly ManualResetEvent _exitSignal;
    private readonly RegisteredWaitHandle _watch;
    private volatile bool _hasExited;
    private bool _disposed;

    public event Action? Exited;

    public int Id { get; }

    public bool HasExited => _hasExited;

    public int ExitCode { get; private set; }

    public EngineProcess(SafeProcessHandle handle, uint id)
    {
        _handle = handle;
        Id = (int)id;
        _exitSignal = new ManualResetEvent(false)
        {
            SafeWaitHandle = new SafeWaitHandle(handle.DangerousGetHandle(), ownsHandle: false),
        };
        _watch = ThreadPool.RegisterWaitForSingleObject(
            _exitSignal, (_, _) => OnExited(), null, Timeout.Infinite, executeOnlyOnce: true);
    }

    public void Kill()
    {
        lock (_gate)
        {
            if (!_disposed)
            {
                // Fails once the process is already gone, which is fine
                PInvoke.TerminateProcess(Handle, 1);
            }
        }
    }

    public void Dispose()
    {
        lock (_gate)
        {
            if (_disposed)
            {
                return;
            }

            _disposed = true;
            _watch.Unregister(null);
            _exitSignal.Dispose();
            _handle.Dispose();
        }
    }

    private HANDLE Handle => new(_handle.DangerousGetHandle());

    private void OnExited()
    {
        lock (_gate)
        {
            if (!_disposed && PInvoke.GetExitCodeProcess(_handle, out var code))
            {
                ExitCode = (int)code;
            }

            _hasExited = true;
        }

        Exited?.Invoke();
    }
}
