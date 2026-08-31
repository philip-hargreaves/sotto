using System.Diagnostics;
using System.Text;
using Sotto.Client;

namespace Sotto.Contract.Tests;

/// <summary>
/// Launches the real sotto_engine.exe and connects a verified client to it.
/// </summary>
internal sealed class EngineProcess : IAsyncDisposable
{
    private const string DefaultPipeName = "LOCAL\\sotto-engine";
    private static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(10);

    private readonly Process _process;
    private readonly string _pipeName;
    private readonly StringBuilder _stderr = new();

    private EngineProcess(Process process, string pipeName, string storeRoot)
    {
        _process = process;
        _pipeName = pipeName;
        StoreRoot = storeRoot;
        // An undrained stderr pipe blocks the engine
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

    // Private pipe and roots per run so tests never touch the app's; a replay
    // wav stands in for the microphone
    public static EngineProcess Start(
        string? pipeName = null, string? replayWavPath = null, string? modelsRoot = null)
    {
        var storeRoot = Path.Combine(Path.GetTempPath(), $"sotto-store-{Guid.NewGuid():N}");
        var startInfo = new ProcessStartInfo(LocateEngine())
        {
            UseShellExecute = false,
            RedirectStandardError = true,
        };
        startInfo.ArgumentList.Add(pipeName ?? DefaultPipeName);
        startInfo.ArgumentList.Add(storeRoot);
        startInfo.ArgumentList.Add(
            modelsRoot ?? Path.Combine(Path.GetTempPath(), $"sotto-models-{Guid.NewGuid():N}"));
        if (replayWavPath is not null)
        {
            startInfo.ArgumentList.Add(replayWavPath);
        }

        var process = Process.Start(startInfo)
            ?? throw new InvalidOperationException("engine failed to start");
        return new EngineProcess(process, pipeName ?? DefaultPipeName, storeRoot);
    }

    public bool IsRunning => !_process.HasExited;

    public string StoreRoot { get; }

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

    // SOTTO_ENGINE_PATH override, else the newest built engine under any preset
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

        try
        {
            Directory.Delete(StoreRoot, recursive: true);
        }
        catch (IOException)
        {
            // A leftover temp store is noise, not a failure
        }
    }
}
