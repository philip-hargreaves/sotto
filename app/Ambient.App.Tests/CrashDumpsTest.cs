using Microsoft.Win32;
using Ambient.App.Core.Hosting;

namespace Ambient.App.Tests;

public sealed class CrashDumpsTest : IDisposable
{
    private const string TestRoot = @"Software\AmbientTests";

    private readonly string _sub = $@"{TestRoot}\{Guid.NewGuid():N}";

    public void Dispose() =>
        Registry.CurrentUser.DeleteSubKeyTree(_sub, throwOnMissingSubKey: false);

    [Fact]
    public void RegistersACappedMinidumpPerProcess()
    {
        using var hive = Registry.CurrentUser.CreateSubKey(_sub);

        CrashDumps.Register(hive, @"C:\dumps", "ambient_engine.exe", "ambient_note_host.exe");

        foreach (var exe in new[] { "ambient_engine.exe", "ambient_note_host.exe" })
        {
            using var key = hive.OpenSubKey(
                $@"SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\{exe}");
            Assert.NotNull(key);
            Assert.Equal(@"C:\dumps", key.GetValue("DumpFolder"));
            Assert.Equal(3, key.GetValue("DumpCount"));
            Assert.Equal(1, key.GetValue("DumpType"));  // never a full memory dump
        }
    }
}
