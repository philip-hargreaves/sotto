using Microsoft.Win32;

namespace Sotto.App.Core.Metrics;

public sealed record GpuInfo(string Name, string Driver);

public sealed record MachineInfo(
    string Cpu,
    int RamGb,
    string Os,
    IReadOnlyList<GpuInfo> Gpus,
    GpuInfo? Npu);

/// <summary>Hardware identity for the performance report; queried once.</summary>
public interface IMachineInfoProvider
{
    MachineInfo Describe();
}

public sealed class WmiMachineInfoProvider : IMachineInfoProvider
{
    private MachineInfo? _cached;

    public MachineInfo Describe() => _cached ??= Query();

    private static MachineInfo Query()
    {
        var cpu = Registry.GetValue(
            @"HKEY_LOCAL_MACHINE\HARDWARE\DESCRIPTION\System\CentralProcessor\0",
            "ProcessorNameString", null) as string ?? "unknown";
        var ramGb = (int)Math.Round(
            GC.GetGCMemoryInfo().TotalAvailableMemoryBytes / (1024.0 * 1024 * 1024));
        // ProductName still says "Windows 10" on Windows 11; the build says
        const string versionKey =
            @"HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion";
        var product = Registry.GetValue(versionKey, "ProductName", "Windows") as string
            ?? "Windows";
        var build = Environment.OSVersion.Version.Build;
        if (build >= 22000)
        {
            product = product.Replace("Windows 10", "Windows 11");
        }

        var display = Registry.GetValue(versionKey, "DisplayVersion", "") as string;
        var os = $"{product} {display} (build {build})".Replace("  ", " ");

        var gpus = new List<GpuInfo>();
        GpuInfo? npu = null;
        try
        {
            using var searcher = new System.Management.ManagementObjectSearcher(
                "SELECT Name, DriverVersion FROM Win32_VideoController");
            foreach (var gpu in searcher.Get())
            {
                gpus.Add(new GpuInfo(
                    gpu["Name"]?.ToString() ?? "unknown",
                    gpu["DriverVersion"]?.ToString() ?? "unknown"));
            }

            using var pnp = new System.Management.ManagementObjectSearcher(
                "SELECT DeviceName, DriverVersion FROM Win32_PnPSignedDriver"
                + " WHERE DeviceName LIKE '%AI Boost%' OR DeviceName LIKE '%NPU%'");
            foreach (var device in pnp.Get())
            {
                npu = new GpuInfo(
                    device["DeviceName"]?.ToString() ?? "NPU",
                    device["DriverVersion"]?.ToString() ?? "unknown");
                break;
            }
        }
        catch (Exception)
        {
        }

        return new MachineInfo(cpu.Trim(), ramGb, os, gpus, npu);
    }
}
