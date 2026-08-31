using Microsoft.Extensions.DependencyInjection;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Ambient.App.Core;
using Ambient.App.Views;

namespace Ambient.App.Services;

/// <summary>
/// Swaps the host's content between DI-resolved views, keeping a back stack of
/// instances so returning to a surface restores it rather than rebuilding it.
/// Views come from the container, so constructor injection holds throughout.
/// </summary>
public sealed class NavigationService(IServiceProvider services) : INavigationService
{
    private static readonly Dictionary<string, Type> Pages = new()
    {
        ["consultation"] = typeof(ConsultationView),
        ["sessions"] = typeof(SessionsView),
        ["settings"] = typeof(SettingsView),
    };

    private readonly Stack<UIElement> _back = new();
    private ContentControl? _host;

    public bool CanGoBack => _back.Count > 0;

    public void Attach(ContentControl host) => _host = host;

    public void NavigateTo(string pageKey)
    {
        var host = Host();
        if (host.Content is UIElement current)
        {
            _back.Push(current);
        }

        host.Content = services.GetRequiredService(Pages[pageKey]);
    }

    public void GoBack()
    {
        if (_back.Count == 0)
        {
            return;
        }

        Host().Content = _back.Pop();
    }

    private ContentControl Host() =>
        _host ?? throw new InvalidOperationException("navigation host not attached");
}
