namespace Sotto.App.Core;

/// <summary>Navigation as a port so view models never touch Frame.</summary>
public interface INavigationService
{
    bool CanGoBack { get; }

    void NavigateTo(string pageKey);

    void GoBack();
}
