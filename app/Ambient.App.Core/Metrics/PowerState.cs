using Microsoft.Win32;
using Windows.Win32;
using Windows.Win32.System.Power;

namespace Ambient.App.Core.Metrics;

/// <summary>
/// The machine's power situation at a moment: the Windows power-mode slider
/// and mains vs battery. Measured attended finalise on the 2-minute consult:
/// 4.0 s on Best power efficiency, 2.8 s on Best performance.
/// </summary>
public sealed record PowerState(string Mode, bool OnMains)
{
    private const string OverlayKey =
        @"SYSTEM\CurrentControlSet\Control\Power\User\PowerSchemes";

    public static PowerState Read()
    {
        var onMains = true;
        var status = new SYSTEM_POWER_STATUS();
        if (PInvoke.GetSystemPowerStatus(out status))
        {
            onMains = status.ACLineStatus != 0;
        }

        var overlay = "";
        try
        {
            using var key = Registry.LocalMachine.OpenSubKey(OverlayKey);
            overlay = key?.GetValue(onMains ? "ActiveOverlayAcPowerScheme" : "ActiveOverlayDcPowerScheme")
                as string ?? "";
        }
        catch (Exception)
        {
        }

        return new PowerState(ModeName(overlay), onMains);
    }

    // The overlay GUIDs are fixed across Windows 10/11
    public static string ModeName(string overlayGuid) => overlayGuid.ToLowerInvariant() switch
    {
        "961cc777-2547-4f9d-8174-7d86181b8a7a" => "efficiency",
        "ded574b5-45a0-4f42-8737-46345c09c238" => "performance",
        "00000000-0000-0000-0000-000000000000" or "" => "balanced",
        _ => "unknown",
    };

    public bool SavingPower => Mode == "efficiency" || !OnMains;
}
