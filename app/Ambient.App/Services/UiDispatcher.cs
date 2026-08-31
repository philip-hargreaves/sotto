using Microsoft.UI.Dispatching;
using Ambient.App.Core;

namespace Ambient.App.Services;

/// <summary>
/// DispatcherQueue adapter. First resolved on the UI thread during launch, so
/// the captured queue is the UI queue.
/// </summary>
public sealed class UiDispatcher : IUiDispatcher
{
    private readonly DispatcherQueue _queue = DispatcherQueue.GetForCurrentThread();

    public void Post(Action action)
    {
        if (!_queue.TryEnqueue(() => action()))
        {
            throw new InvalidOperationException("UI dispatcher queue rejected the work item");
        }
    }
}
