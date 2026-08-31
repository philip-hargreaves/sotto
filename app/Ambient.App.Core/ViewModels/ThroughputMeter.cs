namespace Ambient.App.Core.ViewModels;

/// <summary>
/// Tokens per second over a short rolling window, one Token() per streamed
/// piece. Pure and clock-agnostic: callers pass seconds, tests script them.
/// </summary>
public sealed class ThroughputMeter(double windowSeconds = 2.0)
{
    private readonly Queue<double> _stamps = new();
    private double _frozen;
    private bool _streaming;

    public void Token(double now)
    {
        _streaming = true;
        _frozen = 0;
        _stamps.Enqueue(now);
        Trim(now);
    }

    /// <summary>The stream finished: the last live value holds until Reset.</summary>
    public void End(double now)
    {
        if (_streaming)
        {
            _frozen = Live(now);
            _streaming = false;
            _stamps.Clear();
        }
    }

    public void Reset()
    {
        _stamps.Clear();
        _frozen = 0;
        _streaming = false;
    }

    public bool Streaming => _streaming;

    /// <summary>0 when nothing has streamed; the frozen value after End.</summary>
    public double TokensPerSecond(double now)
    {
        if (!_streaming)
        {
            return _frozen;
        }

        Trim(now);
        return Live(now);
    }

    // Rate over what the window still holds. One token is not a rate yet;
    // a stalled stream decays to zero as the window empties
    private double Live(double now)
    {
        if (_stamps.Count < 2)
        {
            return 0;
        }

        var span = now - _stamps.Peek();
        return span <= 0 ? 0 : (_stamps.Count - 1) / span;
    }

    private void Trim(double now)
    {
        while (_stamps.Count > 0 && now - _stamps.Peek() > windowSeconds)
        {
            _stamps.Dequeue();
        }
    }
}
