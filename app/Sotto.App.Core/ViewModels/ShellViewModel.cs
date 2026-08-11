using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace Sotto.App.Core.ViewModels;

/// <summary>Window-level commands: the routes between the app's surfaces.</summary>
public sealed partial class ShellViewModel : ObservableObject
{
    private readonly INavigationService _navigation;

    public ShellViewModel(INavigationService navigation) => _navigation = navigation;

    [RelayCommand]
    private void ShowSettings() => _navigation.NavigateTo("settings");

    [RelayCommand]
    private void GoBack() => _navigation.GoBack();
}
