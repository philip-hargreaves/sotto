namespace Sotto.App.Core;

/// <summary>
/// Runs an action on the UI thread. Bound state can only be touched from the
/// UI thread, and engine events arrive on a background thread, so view models
/// hand the update to Post instead of applying it directly.
/// </summary>
public interface IUiDispatcher
{
    void Post(Action action);
}
