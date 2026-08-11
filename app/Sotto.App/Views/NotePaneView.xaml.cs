using Microsoft.UI.Xaml.Controls;
using Sotto.App.Core.ViewModels;

namespace Sotto.App.Views;

public sealed partial class NotePaneView : UserControl
{
    public NotePaneView(NoteViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
    }

    public NoteViewModel ViewModel { get; }
}
