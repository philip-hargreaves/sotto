using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Ambient.App.Core.ViewModels;
using Windows.Storage.Pickers;

namespace Ambient.App.Views;

public sealed partial class DemoTrayView : UserControl
{
    public DemoTrayView(DemoTrayViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
    }

    public DemoTrayViewModel ViewModel { get; }

    private async void OnBrowse(object sender, RoutedEventArgs e)
    {
        var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.Downloads };
        picker.FileTypeFilter.Add(".wav");
        // Unpackaged WinUI: the picker must be bound to our window handle
        var window = App.Current.Window;
        if (window is null)
        {
            return;
        }

        WinRT.Interop.InitializeWithWindow.Initialize(
            picker, WinRT.Interop.WindowNative.GetWindowHandle(window));
        var file = await picker.PickSingleFileAsync();
        if (file is not null)
        {
            ViewModel.UseTrack(file.Path);
        }
    }
}
