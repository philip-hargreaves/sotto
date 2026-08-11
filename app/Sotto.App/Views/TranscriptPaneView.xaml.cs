using Microsoft.UI.Xaml.Controls;
using Sotto.App.Core.ViewModels;

namespace Sotto.App.Views;

public sealed partial class TranscriptPaneView : UserControl
{
    public TranscriptPaneView(TranscriptViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
    }

    public TranscriptViewModel ViewModel { get; }
}
