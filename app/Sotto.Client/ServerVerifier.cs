using System.IO.Pipes;
using System.Runtime.InteropServices;

namespace Sotto.Client;

/// <summary>
/// Reads the pid of the process serving a named pipe. The spawner knows which
/// engine pid it started and compares, which needs no signature and cannot be
/// spoofed by renaming a binary.
/// </summary>
public static partial class ServerVerifier
{
    public static uint? GetServerProcessId(NamedPipeClientStream pipe) =>
        GetNamedPipeServerProcessId(pipe.SafePipeHandle.DangerousGetHandle(), out var pid)
            ? pid
            : null;

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool GetNamedPipeServerProcessId(nint pipeHandle, out uint serverPid);
}
