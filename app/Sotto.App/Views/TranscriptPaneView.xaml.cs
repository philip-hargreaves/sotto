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
        // Presentation only: the empty hint and keeping the newest turn in view
        viewModel.Turns.CollectionChanged += OnTurnsChanged;
    }

    public TranscriptViewModel ViewModel { get; }

    public bool EmptyVisible => ViewModel.Turns.Count == 0;

    private void OnTurnsChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        Bindings.Update();
        if (ViewModel.Turns.Count > 0)
        {
            TurnList.ScrollIntoView(ViewModel.Turns[^1]);
        }
    }
}
