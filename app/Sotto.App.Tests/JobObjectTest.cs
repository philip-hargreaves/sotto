using System.Diagnostics;
using Sotto.App.Core.Hosting;

namespace Sotto.App.Tests;

public class JobObjectTest
{
    // A process that stays alive without needing a console or stdin.
    private static Process StartWaiter() =>
        Process.Start(new ProcessStartInfo("ping", "-n 60 127.0.0.1")
        {
            UseShellExecute = false,
            CreateNoWindow = true,
        })!;

    [Fact]
    public async Task DisposingTheJobKillsAnAssignedProcess()
    {
        using var waiter = StartWaiter();
        try
        {
            var job = new JobObject();
            job.Assign(waiter);
            Assert.False(waiter.HasExited);

            job.Dispose();

            // KILL_ON_JOB_CLOSE fires when the last handle closes.
            await waiter.WaitForExitAsync(new CancellationTokenSource(TimeSpan.FromSeconds(5)).Token);
            Assert.True(waiter.HasExited);
        }
        finally
        {
            if (!waiter.HasExited)
            {
                waiter.Kill();
            }
        }
    }
}
