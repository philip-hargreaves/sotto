using Sotto.App.Core.ViewModels;

namespace Sotto.App.Tests;

public class NavigationTest
{
    [Fact]
    public void ShowSettingsNavigatesThroughThePort()
    {
        var navigation = new RecordingNavigationService();
        var shell = new ShellViewModel(navigation);

        shell.ShowSettingsCommand.Execute(null);

        Assert.Equal("settings", navigation.Current);
    }

    [Fact]
    public void GoBackReturnsToThePreviousSurface()
    {
        var navigation = new RecordingNavigationService();
        navigation.NavigateTo("consultation");
        var shell = new ShellViewModel(navigation);

        shell.ShowSettingsCommand.Execute(null);
        shell.GoBackCommand.Execute(null);

        Assert.Equal("consultation", navigation.Current);
        Assert.False(navigation.CanGoBack);
    }
}
