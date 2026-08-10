using System.Diagnostics;
using System.IO.Pipes;
using System.Runtime.InteropServices;

namespace Sotto.Client;

/// <summary>
/// Confirms the connected pipe server is the expected engine binary before the
/// channel is trusted. Authenticode verification is deferred until binaries are
/// signed; unsigned dev builds would always fail it.
/// </summary>
public static partial class ServerVerifier
{
    public static bool ServerImageMatches(NamedPipeClientStream pipe, string expectedFileName)
    {
        if (!GetNamedPipeServerProcessId(pipe.SafePipeHandle.DangerousGetHandle(), out var pid))
        {
            return false;
        }

        try
        {
            using var process = Process.GetProcessById((int)pid);
            var path = process.MainModule?.FileName;
            return path is not null
                && string.Equals(Path.GetFileName(path), expectedFileName,
                    StringComparison.OrdinalIgnoreCase);
        }
        catch (ArgumentException)
        {
            return false;
        }
    }

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool GetNamedPipeServerProcessId(nint pipeHandle, out uint serverPid);
}
