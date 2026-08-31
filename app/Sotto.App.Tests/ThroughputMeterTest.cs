using Sotto.App.Core.ViewModels;

namespace Sotto.App.Tests;

public class ThroughputMeterTest
{
    [Fact]
    public void ASteadyStreamReadsItsRate()
    {
        var meter = new ThroughputMeter();
        for (var i = 0; i <= 20; i++)
        {
            meter.Token(i * 0.05);  // 20 tokens per second
        }

        Assert.True(meter.Streaming);
        Assert.Equal(20.0, meter.TokensPerSecond(1.0), 1);
    }

    [Fact]
    public void TheWindowRollsSoASpeedChangeShowsThrough()
    {
        var meter = new ThroughputMeter(windowSeconds: 2.0);
        for (var i = 0; i < 20; i++)
        {
            meter.Token(i * 0.1);  // 10 tok/s for two seconds
        }

        Assert.Equal(10.0, meter.TokensPerSecond(2.0), 0);

        for (var i = 0; i <= 50; i++)
        {
            meter.Token(2.0 + i * 0.02);  // then 50 tok/s for one second
        }

        // Once the fast phase fills the window, the slow tokens have aged out
        Assert.True(meter.TokensPerSecond(3.1) > 25, "the rate follows the stream");
    }

    [Fact]
    public void AStallDecaysToZeroAsTheWindowEmpties()
    {
        var meter = new ThroughputMeter(windowSeconds: 2.0);
        meter.Token(0.0);
        meter.Token(0.1);
        meter.Token(0.2);

        Assert.Equal(0, meter.TokensPerSecond(5.0));
        Assert.True(meter.Streaming, "stalled is not finished");
    }

    [Fact]
    public void EndFreezesTheLastValueAndResetClearsIt()
    {
        var meter = new ThroughputMeter();
        for (var i = 0; i <= 10; i++)
        {
            meter.Token(i * 0.1);  // 10 tok/s
        }

        meter.End(1.0);
        Assert.False(meter.Streaming);
        Assert.Equal(10.0, meter.TokensPerSecond(60.0), 1);  // holds, hours later

        meter.Reset();
        Assert.Equal(0, meter.TokensPerSecond(60.0));
    }

    [Fact]
    public void OneTokenIsNotARateAndEndWithoutStreamingIsQuiet()
    {
        var meter = new ThroughputMeter();
        meter.Token(0.0);
        Assert.Equal(0, meter.TokensPerSecond(0.5));

        var idle = new ThroughputMeter();
        idle.End(1.0);
        Assert.Equal(0, idle.TokensPerSecond(2.0));
        Assert.False(idle.Streaming);
    }

    [Fact]
    public void ANewStreamAfterEndMetersFreshly()
    {
        var meter = new ThroughputMeter();
        for (var i = 0; i <= 10; i++)
        {
            meter.Token(i * 0.1);
        }

        meter.End(1.0);

        for (var i = 0; i <= 10; i++)
        {
            meter.Token(10.0 + i * 0.02);  // 50 tok/s regeneration
        }

        Assert.True(meter.Streaming);
        Assert.Equal(50.0, meter.TokensPerSecond(10.2), 0);
    }
}
