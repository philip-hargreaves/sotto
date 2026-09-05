using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace Ambient.App.Controls;

/// <summary>
/// The presence behind a disc button: a soft accent glow with no edge that
/// brightens and reaches further with the microphone level, and one hairline
/// ring that eases outward from the disc. Restrained on purpose: it should read
/// as "alive", never as an alarm.
/// </summary>
public sealed partial class LevelRing : UserControl
{
    private const double GlowReach = 0.6;    // the glow's diameter beyond the disc at full level, as a fraction
    private const double RingReach = 0.22;   // the ring's swell at full level
    private const double GlowFloor = 0.08;   // glow alpha at silence
    private const double GlowCeiling = 0.42;
    private const double RingFloor = 0.25;   // ring alpha at silence
    private const double RingCeiling = 0.85;

    public static readonly DependencyProperty LevelProperty = DependencyProperty.Register(
        nameof(Level), typeof(double), typeof(LevelRing),
        new PropertyMetadata(0.0, (d, _) => ((LevelRing)d).Apply()));

    public static readonly DependencyProperty DiameterProperty = DependencyProperty.Register(
        nameof(Diameter), typeof(double), typeof(LevelRing),
        new PropertyMetadata(88.0, (d, _) => ((LevelRing)d).Apply()));

    private readonly RadialGradientBrush _glow = new();
    private readonly SolidColorBrush _ring = new();
    private Color _accent = Colors.CornflowerBlue;

    public LevelRing()
    {
        InitializeComponent();
        if (Application.Current.Resources.TryGetValue("ColorAccent", out var accent) && accent is Color colour)
        {
            _accent = colour;
        }

        _glow.GradientStops.Add(new GradientStop { Offset = 0.0 });
        _glow.GradientStops.Add(new GradientStop { Offset = 0.55 });
        _glow.GradientStops.Add(new GradientStop { Offset = 1.0, Color = Transparent() });
        Glow.Fill = _glow;
        Ring.Stroke = _ring;
        ActualThemeChanged += (_, _) =>
        {
            if (Application.Current.Resources.TryGetValue("ColorAccent", out var a) && a is Color c)
            {
                _accent = c;
            }

            Apply();
        };
        Apply();
    }

    /// <summary>Microphone level, 0 to 1.</summary>
    public double Level
    {
        get => (double)GetValue(LevelProperty);
        set => SetValue(LevelProperty, value);
    }

    /// <summary>The disc the ring sits behind.</summary>
    public double Diameter
    {
        get => (double)GetValue(DiameterProperty);
        set => SetValue(DiameterProperty, value);
    }

    // Pure, so a test can pin the curve without a visual tree
    public static double GlowScaleFor(double level) => 1.0 + GlowReach * Math.Clamp(level, 0.0, 1.0);

    public static double RingScaleFor(double level) => 1.0 + RingReach * Math.Clamp(level, 0.0, 1.0);

    public static double GlowAlphaFor(double level) =>
        GlowFloor + (GlowCeiling - GlowFloor) * Math.Clamp(level, 0.0, 1.0);

    public static double RingAlphaFor(double level) =>
        RingFloor + (RingCeiling - RingFloor) * Math.Clamp(level, 0.0, 1.0);

    private Color Transparent() => Color.FromArgb(0, _accent.R, _accent.G, _accent.B);

    private Color WithAlpha(double alpha) =>
        Color.FromArgb((byte)Math.Round(255 * Math.Clamp(alpha, 0.0, 1.0)), _accent.R, _accent.G, _accent.B);

    private void Apply()
    {
        var level = Level;
        Glow.Width = Glow.Height = Diameter;
        Ring.Width = Ring.Height = Diameter + 6;
        GlowScale.ScaleX = GlowScale.ScaleY = GlowScaleFor(level);
        RingScale.ScaleX = RingScale.ScaleY = RingScaleFor(level);
        _glow.GradientStops[0].Color = WithAlpha(GlowAlphaFor(level));
        _glow.GradientStops[1].Color = WithAlpha(GlowAlphaFor(level) * 0.45);
        _glow.GradientStops[2].Color = Transparent();
        _ring.Color = WithAlpha(RingAlphaFor(level));
    }
}
