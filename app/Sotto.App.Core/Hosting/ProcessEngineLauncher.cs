using System.ComponentModel;
using Microsoft.Win32.SafeHandles;
using Windows.Win32;
using Windows.Win32.System.Threading;

namespace Sotto.App.Core.Hosting;

/// <summary>
/// Launches the engine race-free: created suspended, assigned to a
/// kill-on-close job, then resumed. Nothing it spawns can escape the job.
/// </summary>
public sealed class ProcessEngineLauncher(string exePath, string arguments = "")
    : IEngineLauncher, IDisposable
{
    private readonly JobObject _job = new();

    public IEngineProcess Launch()
    {
        // CreateProcess may write into the command line, so it needs a buffer
        Span<char> commandLine = ($"\"{exePath}\" {arguments}".TrimEnd() + '\0').ToCharArray();

        unsafe
        {
            var startup = new STARTUPINFOW { cb = (uint)sizeof(STARTUPINFOW) };
            if (!PInvoke.CreateProcess(
                    exePath, ref commandLine, null, null, false,
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

    public void Dispose() => _job.Dispose();
}
