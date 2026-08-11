using Microsoft.UI.Xaml.Controls;
using Sotto.App.Core.ViewModels;

namespace Sotto.App.Views;

public sealed partial class SessionControlsView : UserControl
{
    public SessionControlsView(SessionControlsViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
    }

    public SessionControlsViewModel ViewModel { get; }
}
