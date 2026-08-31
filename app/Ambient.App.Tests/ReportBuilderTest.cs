using System.Text.Json;
using Sotto.App.Core.Metrics;

namespace Sotto.App.Tests;

public class ReportBuilderTest
{
    private static readonly MachineInfo Machine = new(
        "Intel(R) Core(TM) Ultra 7 258V", 32, "Windows 11 26200",
        [new GpuInfo("Intel(R) Arc(TM) 140T GPU", "32.0.101.6083")],
        new GpuInfo("Intel(R) AI Boost", "32.0.100.3104"));

    private static string Session(string asr, double rtf, double? replaySpeed) =>
        JsonSerializer.Serialize(new
        {
            start = "2026-08-19T21:12:44Z",
            source = replaySpeed is null ? "mic" : "replay",
            replaySpeed,
            track = replaySpeed is null ? null : "Elbow swelling",
            engine = new
            {
                devices = new { asr },
                asrRealtimeFactor = rtf,
                stageSeconds = new Dictionary<string, double> { ["transcript sealed"] = 2.0 },
                loadSeconds = new { asr = 2.1 },
                audioSeconds = 541.5,
            },
            note = new { firstPartialAfterStopSeconds = 15.1, readyAfterStopSeconds = 31.3, chars = 1290 },
            memory = new { peakWorkingSetMb = 9800, availableAtStartMb = 21000 },
        });

    [Fact]
    public void RendersMachineSessionsAndEmbeddedJson()
    {
        var html = ReportBuilder.Build(Machine,
            [Session("GPU.0", 33.4, 1.0)], DateTimeOffset.UtcNow);

        Assert.Contains("Intel(R) Core(TM) Ultra 7 258V", html);
        Assert.Contains("Elbow swelling", html);
        Assert.Contains("33.4", html);
        var json = html.Split("type=\"application/json\">")[1].Split("</script>")[0];
        using var embedded = JsonDocument.Parse(json);
        Assert.Equal(1, embedded.RootElement.GetProperty("sessions").GetArrayLength());
    }

    [Fact]
    public void RendersPowerModeAndEngineThrottling()
    {
        var session = JsonSerializer.Serialize(new
        {
            start = "2026-08-29T13:40:17Z",
            source = "replay",
            replaySpeed = 1.0,
            track = "Elbow swelling",
            engine = new { devices = new { asr = "GPU.0" }, powerThrottling = "off" },
            power = new { mode = "performance", onMains = true },
            note = new { chars = 500 },
            memory = new { },
        });

        var html = ReportBuilder.Build(Machine, [session], DateTimeOffset.UtcNow);

        // The separator is HTML-encoded; assert the three facts
        var machine = html.Split("<h2>Machine</h2>")[1].Split("</table>")[0];
        Assert.Contains("performance mode", machine);
        Assert.Contains("mains", machine);
        Assert.Contains("engine throttling off", machine);
    }

    [Fact]
    public void PowerModeNamesTheSliderOverlays()
    {
        Assert.Equal("efficiency", PowerState.ModeName("961CC777-2547-4F9D-8174-7D86181B8A7A"));
        Assert.Equal("performance", PowerState.ModeName("ded574b5-45a0-4f42-8737-46345c09c238"));
        Assert.Equal("balanced", PowerState.ModeName(""));
        Assert.Equal("balanced", PowerState.ModeName("00000000-0000-0000-0000-000000000000"));
        Assert.Equal("unknown", PowerState.ModeName("not-a-guid"));
        Assert.True(new PowerState("efficiency", true).SavingPower);
        Assert.True(new PowerState("performance", false).SavingPower);
        Assert.False(new PowerState("performance", true).SavingPower);
    }

    [Fact]
    public void ComparesDevicesOnlyWhenTwoHaveComparableSessions()
    {
        var gpuOnly = ReportBuilder.Build(Machine,
            [Session("GPU.0", 33.4, 1.0)], DateTimeOffset.UtcNow);
        Assert.DoesNotContain("Transcription by Device", gpuOnly);

        var both = ReportBuilder.Build(Machine,
            [Session("GPU.0", 33.4, 1.0), Session("NPU", 11.2, 1.0)], DateTimeOffset.UtcNow);
        Assert.Contains("Transcription by Device", both);
        Assert.Contains("11.2", both);
    }

    [Fact]
    public void AcceleratedReplaysAreExcludedFromComparison()
    {
        var html = ReportBuilder.Build(Machine,
            [Session("GPU.0", 33.4, 1.0), Session("NPU", 11.2, 16.0)], DateTimeOffset.UtcNow);

        Assert.Contains("Replay &#215;16", html);  // × html-encoded
        Assert.DoesNotContain("Transcription by Device", html);
    }

    [Fact]
    public void AGarbledLineIsSkippedNotFatal()
    {
        var html = ReportBuilder.Build(Machine,
            ["not json at all", Session("GPU.0", 33.4, null)], DateTimeOffset.UtcNow);
        Assert.Contains("1 recorded sessions", html);
    }
}
