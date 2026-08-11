using System.Diagnostics;
using System.Text;
using Sotto.Client;

namespace Sotto.Contract.Tests;

/// <summary>
/// Launches the real sotto_engine.exe and connects a verified client to it.
/// Naive spawn-and-kill: process supervision is a later concern, this only
/// needs a live engine to talk to.
/// </summary>
internal sealed class EngineProcess : IAsyncDisposable
{
    private const string DefaultPipeName = "LOCAL\\sotto-engine";
    private static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(10);

    private readonly Process _process;
    private readonly string _pipeName;
    private readonly StringBuilder _stderr = new();

    private EngineProcess(Process process, string pipeName)
    {
        _process = process;
        _pipeName = pipeName;
        // Drain stderr continuously: an undrained pipe fills and blocks the
        // engine's own logging, and the text is the only startup diagnostic.
        _process.ErrorDataReceived += (_, e) =>
        {
            if (e.Data is not null)
            {
                lock (_stderr)
                {
                    _stderr.AppendLine(e.Data);
                }
            }
        };
        _process.BeginErrorReadLine();
    }

    // A private pipe name keeps a test off the fixed one, so it neither waits on
    // the engine collection nor collides with an app running on this machine.
    public static EngineProcess Start(string? pipeName = null)
    {
        var startInfo = new ProcessStartInfo(LocateEngine())
        {
            UseShellExecute = false,
            RedirectStandardError = true,
        };
        if (pipeName is not null)
        {
            startInfo.ArgumentList.Add(pipeName);
        }

        var process = Process.Start(startInfo)
            ?? throw new InvalidOperationException("engine failed to start");
        return new EngineProcess(process, pipeName ?? DefaultPipeName);
    }

    public bool IsRunning => !_process.HasExited;

    public string StandardError
    {
        get
        {
            lock (_stderr)
            {
                return _stderr.ToString();
            }
        }
    }

    public async Task<int> WaitForExitAsync(TimeSpan timeout)
    {
        using var cts = new CancellationTokenSource(timeout);
        await _process.WaitForExitAsync(cts.Token).ConfigureAwait(false);
        return _process.ExitCode;
    }

    public Task<PipeTransport> ConnectAsync()
    {
        if (_process.HasExited)
        {
            throw new InvalidOperationException(
                $"engine exited (code {_process.ExitCode}) before a client could connect: {StandardError}");
        }

        return PipeTransport.ConnectAsync(_pipeName, ConnectTimeout, (uint)_process.Id);
    }

    // The engine is built by CMake, not copied beside these tests, so its path is
    // resolved: an explicit override, else the most recently built binary under
    // any preset (newest wins, so a fresh build is never shadowed by a stale one).
    private static string LocateEngine()
    {
        const string exe = "sotto_engine.exe";
        var overridePath = Environment.GetEnvironmentVariable("SOTTO_ENGINE_PATH");
        if (!string.IsNullOrEmpty(overridePath))
        {
            return overridePath;
        }

        var dir = AppContext.BaseDirectory;
        while (dir is not null)
        {
            var buildRoot = Path.Combine(dir, "build");
            if (Directory.Exists(buildRoot))
            {
                var newest = Directory
                    .EnumerateFiles(buildRoot, exe, SearchOption.AllDirectories)
                    .Select(path => new FileInfo(path))
                    .OrderByDescending(info => info.LastWriteTimeUtc)
                    .FirstOrDefault();
                if (newest is not null)
                {
                    return newest.FullName;
                }
            }

            dir = Path.GetDirectoryName(dir);
        }

        throw new FileNotFoundException(
            $"{exe} not found via SOTTO_ENGINE_PATH or under build/");
    }

    public async ValueTask DisposeAsync()
    {
        if (!_process.HasExited)
        {
            _process.Kill();
        }

        await _process.WaitForExitAsync().ConfigureAwait(false);
        _process.Dispose();
    }
}
