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
        // Chip visibility is computed here; the async model fetch needs a nudge
        viewModel.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(StatusBarViewModel.AsrChip)
                or nameof(StatusBarViewModel.NoteChip)
                or nameof(StatusBarViewModel.MemoryChip)
                or nameof(StatusBarViewModel.MetricsVisible))
            {
                Bindings.Update();
            }
        };
    }

    public StatusBarViewModel ViewModel { get; }

    public bool AsrChipVisible => ViewModel.MetricsVisible && ViewModel.AsrChip.Length > 0;

    public bool NoteChipVisible => ViewModel.MetricsVisible && ViewModel.NoteChip.Length > 0;

    public bool MemoryChipVisible => ViewModel.MetricsVisible && ViewModel.MemoryChip.Length > 0;

    [System.Diagnostics.CodeAnalysis.SuppressMessage("Performance", "CA1822",
        Justification = "x:Bind function bindings require an instance member")]
    public Microsoft.UI.Xaml.Media.Brush AsrForeground(bool low) =>
        (Microsoft.UI.Xaml.Media.Brush)Application.Current.Resources[
            low ? "CautionBrush" : "TextFillColorSecondaryBrush"];

    // Idle is not a fault, so the resting dot is a quiet neutral, never red
    [System.Diagnostics.CodeAnalysis.SuppressMessage("Performance", "CA1822",
        Justification = "x:Bind function bindings require an instance member")]
    public Microsoft.UI.Xaml.Media.Brush DotFill(bool active) =>
        (Microsoft.UI.Xaml.Media.Brush)Application.Current.Resources[
            active ? "SystemFillColorSuccessBrush" : "ControlStrongFillColorDisabledBrush"];

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
