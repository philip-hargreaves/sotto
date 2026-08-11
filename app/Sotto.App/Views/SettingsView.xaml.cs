using Microsoft.UI.Xaml.Controls;
using Sotto.App.Core.ViewModels;

namespace Sotto.App.Views;

public sealed partial class SettingsView : UserControl
{
    public SettingsView(SettingsViewModel viewModel, ShellViewModel shell)
    {
        ViewModel = viewModel;
        Shell = shell;
        InitializeComponent();
    }

    public SettingsViewModel ViewModel { get; }

    public ShellViewModel Shell { get; }
}
