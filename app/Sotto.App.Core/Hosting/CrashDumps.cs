using Microsoft.Win32;

namespace Sotto.App.Core.Hosting;

/// <summary>
/// Opts the engine processes into Windows Error Reporting local dumps:
/// per-user registry, no elevation, minidumps only (never full memory - a
/// full dump could carry consultation audio), a capped count. Idempotent,
/// re-asserted every launch so a support bundle always has the mechanism.
/// </summary>
public static class CrashDumps
{
    private const string LocalDumps =
        @"SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps";

    public static void Register(RegistryKey hive, string dumpFolder, params string[] exeNames)
    {
        foreach (var exe in exeNames)
        {
            try
            {
                using var key = hive.CreateSubKey($@"{LocalDumps}\{exe}");
                key.SetValue("DumpFolder", dumpFolder, RegistryValueKind.ExpandString);
                key.SetValue("DumpCount", 3, RegistryValueKind.DWord);
                key.SetValue("DumpType", 1, RegistryValueKind.DWord);  // mini, not full
            }
            catch (Exception)
            {
                // Diagnostics must never block startup
            }
        }
    }
}
