using Microsoft.UI.Xaml.Controls;
using Ambient.App.Core.ViewModels;

namespace Ambient.App.Views;

public sealed partial class SessionControlsView : UserControl
{
    public SessionControlsView(SessionControlsViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
    }

    public SessionControlsViewModel ViewModel { get; }
}
