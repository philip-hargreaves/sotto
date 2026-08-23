using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;
using Sotto.App.Core.ViewModels;

namespace Sotto.App.Views;

public sealed partial class StatusBarView : UserControl
{
    private readonly CreditsViewModel _credits;

    public StatusBarView(StatusBarViewModel viewModel, CreditsViewModel credits)
    {
        ViewModel = viewModel;
        _credits = credits;
        InitializeComponent();
        // Presentation only: the marks re-resolve when the theme changes
        BuildCredits();
        ActualThemeChanged += (_, _) => BuildCredits();
    }

    public StatusBarViewModel ViewModel { get; }

    private void BuildCredits()
    {
        CreditsRow.Children.Clear();
        var dark = ActualTheme == ElementTheme.Dark;
        foreach (var mark in _credits.Marks)
        {
            var path = dark ? mark.DarkPath : mark.LightPath;
            var uri = new Uri(path);
            CreditsRow.Children.Add(new Image
            {
                Height = mark.Height,
                Source = path.EndsWith(".svg", StringComparison.OrdinalIgnoreCase)
                    ? new SvgImageSource(uri)
                    : new BitmapImage(uri),
                VerticalAlignment = VerticalAlignment.Center,
            });
        }
    }
}
