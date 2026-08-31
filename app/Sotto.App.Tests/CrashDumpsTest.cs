using Microsoft.Win32;
using Sotto.App.Core.Hosting;

namespace Sotto.App.Tests;

public sealed class CrashDumpsTest : IDisposable
{
    private const string TestRoot = @"Software\SottoTests";

    private readonly string _sub = $@"{TestRoot}\{Guid.NewGuid():N}";

    public void Dispose() =>
        Registry.CurrentUser.DeleteSubKeyTree(_sub, throwOnMissingSubKey: false);

    [Fact]
    public void RegistersACappedMinidumpPerProcess()
    {
        using var hive = Registry.CurrentUser.CreateSubKey(_sub);

        CrashDumps.Register(hive, @"C:\dumps", "sotto_engine.exe", "sotto_note_host.exe");

        foreach (var exe in new[] { "sotto_engine.exe", "sotto_note_host.exe" })
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
