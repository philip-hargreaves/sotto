using System.Collections.Specialized;
using Microsoft.UI.Xaml.Controls;
using Ambient.App.Core.ViewModels;

namespace Ambient.App.Views;

public sealed partial class TranscriptPaneView : UserControl
{
    public TranscriptPaneView(TranscriptViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
        // Presentation only: keep the newest turn in view
        viewModel.Turns.CollectionChanged += OnTurnsChanged;
        // Realised items keep old brushes; a theme change or re-attach after an
        // off-tree change re-realises the list
        SpeakerPalette.Theme = ActualTheme;
        ActualThemeChanged += (_, _) => RefreshStripes();
        Loaded += (_, _) => RefreshStripes();
    }

    private void RefreshStripes()
    {
        if (SpeakerPalette.Theme == ActualTheme)
        {
            return;
        }

        SpeakerPalette.Theme = ActualTheme;
        TurnList.ItemsSource = null;
        TurnList.ItemsSource = ViewModel.Turns;
    }

    public TranscriptViewModel ViewModel { get; }

    private void OnTurnsChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        if (ViewModel.Turns.Count > 0)
        {
            TurnList.ScrollIntoView(ViewModel.Turns[^1]);
        }
    }
}
