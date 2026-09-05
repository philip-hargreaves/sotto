using System.ComponentModel;
using Microsoft.Win32.SafeHandles;
using Windows.Win32;
using Windows.Win32.System.Threading;

namespace Ambient.App.Core.Hosting;

/// <summary>
/// Launches the engine race-free: created suspended, assigned to a
/// kill-on-close job, then resumed. Nothing it spawns can escape the job.
/// Engine stderr goes to a log file so failures are diagnosable.
/// </summary>
public sealed class ProcessEngineLauncher(string exePath, string arguments = "",
    string? stderrPath = null, Func<string>? extraArguments = null)
    : IEngineLauncher, IDisposable
{
    private readonly JobObject _job = new();
    private bool _rotated;

    public IEngineProcess Launch()
    {
        // Extra arguments are read per launch, so a restart picks up changes
        var extra = extraArguments?.Invoke() ?? "";
        // CreateProcess may write into the command line, so it needs a buffer
        Span<char> commandLine =
            ($"\"{exePath}\" {arguments} {extra}".TrimEnd() + '\0').ToCharArray();

        using var stderr = OpenStderr();
        unsafe
        {
            var startup = new STARTUPINFOW { cb = (uint)sizeof(STARTUPINFOW) };
            if (stderr is not null)
            {
                startup.dwFlags = STARTUPINFOW_FLAGS.STARTF_USESTDHANDLES;
                startup.hStdError = new global::Windows.Win32.Foundation.HANDLE(
                    stderr.SafeFileHandle.DangerousGetHandle());
            }

            if (!PInvoke.CreateProcess(
                    exePath, ref commandLine, null, null, stderr is not null,
                    PROCESS_CREATION_FLAGS.CREATE_SUSPENDED | PROCESS_CREATION_FLAGS.CREATE_NO_WINDOW,
                    null,
                    Path.GetDirectoryName(exePath), in startup, out var info))
            {
                throw new Win32Exception();
            }

            var handle = new SafeProcessHandle((IntPtr)info.hProcess.Value, ownsHandle: true);
            try
            {
                _job.Assign(handle);
                if (OperatingSystem.IsWindowsVersionAtLeast(8))
                {
                    PowerThrottling.Disable(handle);  // the engine repeats this on itself
                }
                if (PInvoke.ResumeThread(info.hThread) == uint.MaxValue)
                {
                    throw new Win32Exception();
                }
            }
            catch
            {
                // A suspended orphan would hang forever, so take it down here
                PInvoke.TerminateProcess(info.hProcess, 1);
                handle.Dispose();
                throw;
            }
            finally
            {
                PInvoke.CloseHandle(info.hThread);
            }

            return new EngineProcess(handle, info.dwProcessId);
        }
    }

    // Appended across launches, rotated once per run; the handle is marked
    // inheritable for the child
    private FileStream? OpenStderr()
    {
        if (stderrPath is null)
        {
            return null;
        }

        if (!_rotated)
        {
            LogRotation.Rotate(stderrPath, keep: 12);
            _rotated = true;
        }

        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(stderrPath)!);
            var stream = new FileStream(
                stderrPath, FileMode.Append, FileAccess.Write, FileShare.ReadWrite);
            PInvoke.SetHandleInformation(
                stream.SafeFileHandle,
                (uint)global::Windows.Win32.Foundation.HANDLE_FLAGS.HANDLE_FLAG_INHERIT,
                global::Windows.Win32.Foundation.HANDLE_FLAGS.HANDLE_FLAG_INHERIT);
            return stream;
        }
        catch (IOException)
        {
            return null;
        }
    }

    public void Dispose() => _job.Dispose();
}
