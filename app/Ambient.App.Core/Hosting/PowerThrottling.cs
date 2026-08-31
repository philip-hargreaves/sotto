using Microsoft.Win32.SafeHandles;
using Windows.Win32;
using Windows.Win32.System.Threading;

namespace Sotto.App.Core.Hosting;

/// <summary>
/// Windows throttles windowless background processes (EcoQoS) once the user
/// is idle; measured finalise 4.0 s to 6.3 s. The launched engine is opted
/// out here as well as by itself, so the state never rests on one call.
/// </summary>
[System.Runtime.Versioning.SupportedOSPlatform("windows8.0")]
public static class PowerThrottling
{
    public static unsafe bool Disable(SafeProcessHandle process)
    {
        var state = new PROCESS_POWER_THROTTLING_STATE
        {
            Version = PInvoke.PROCESS_POWER_THROTTLING_CURRENT_VERSION,
            ControlMask = PInvoke.PROCESS_POWER_THROTTLING_EXECUTION_SPEED,
            StateMask = 0,
        };
        return PInvoke.SetProcessInformation(
            new global::Windows.Win32.Foundation.HANDLE(process.DangerousGetHandle()),
            PROCESS_INFORMATION_CLASS.ProcessPowerThrottling, &state,
            (uint)sizeof(PROCESS_POWER_THROTTLING_STATE));
    }

    /// <summary>"off", "on", "default" (Windows decides) or "unknown".</summary>
    public static unsafe string Describe(SafeProcessHandle process)
    {
        var state = new PROCESS_POWER_THROTTLING_STATE
        {
            Version = PInvoke.PROCESS_POWER_THROTTLING_CURRENT_VERSION,
        };
        if (!PInvoke.GetProcessInformation(
                new global::Windows.Win32.Foundation.HANDLE(process.DangerousGetHandle()),
                PROCESS_INFORMATION_CLASS.ProcessPowerThrottling, &state,
                (uint)sizeof(PROCESS_POWER_THROTTLING_STATE)))
        {
            return "unknown";
        }

        if ((state.ControlMask & PInvoke.PROCESS_POWER_THROTTLING_EXECUTION_SPEED) == 0)
        {
            return "default";
        }

        return (state.StateMask & PInvoke.PROCESS_POWER_THROTTLING_EXECUTION_SPEED) != 0
            ? "on" : "off";
    }
}
