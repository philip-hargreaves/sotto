using System.Text.Json;
using Sotto.App.Core.Hosting;

namespace Sotto.App.Tests;

public class FileCrashLogTest
{
    private static readonly CrashReport Report = new(
        DateTimeOffset.UnixEpoch, -1073741819, TimeSpan.FromSeconds(90), 1,
        RecoveryAction.Restart);

    [Fact]
    public void AppendsOneParseableLinePerReport()
    {
        var directory = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        var path = Path.Combine(directory, "crashes.jsonl");
        try
        {
            var log = new FileCrashLog(path);

            log.Record(Report);
            log.Record(Report with { CrashCount = 2, Action = RecoveryAction.GiveUp });

            var lines = File.ReadAllLines(path);
            Assert.Equal(2, lines.Length);
            using var last = JsonDocument.Parse(lines[1]);
            Assert.Equal(-1073741819, last.RootElement.GetProperty("ExitCode").GetInt32());
            Assert.Equal(2, last.RootElement.GetProperty("CrashCount").GetInt32());
            Assert.Equal("GiveUp", last.RootElement.GetProperty("Action").GetString());
        }
        finally
        {
            if (Directory.Exists(directory))
            {
                Directory.Delete(directory, recursive: true);
            }
        }
    }

    [Fact]
    public void WriteFailuresNeverThrow()
    {
        var log = new FileCrashLog("\0not-a-path");

        log.Record(Report);
    }
}
