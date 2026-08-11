using Microsoft.UI.Xaml.Controls;
using Sotto.App.Core.ViewModels;

namespace Sotto.App.Views;

public sealed partial class StatusBarView : UserControl
{
    public StatusBarView(StatusBarViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
    }

    public StatusBarViewModel ViewModel { get; }
}
