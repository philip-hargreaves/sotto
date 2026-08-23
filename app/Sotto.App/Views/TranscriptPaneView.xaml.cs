using System.Collections.Specialized;
using Microsoft.UI.Xaml.Controls;
using Sotto.App.Core.ViewModels;

namespace Sotto.App.Views;

public sealed partial class TranscriptPaneView : UserControl
{
    public TranscriptPaneView(TranscriptViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
        // Presentation only: keep the newest turn in view
        viewModel.Turns.CollectionChanged += OnTurnsChanged;
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
