namespace Sotto.App.Core;

/// <summary>
/// Marshals work onto the UI thread. Engine notifications arrive on the
/// transport's read loop, and neither the transport nor the Messenger changes
/// threads, so every path from the engine into bound state crosses this port.
/// </summary>
public interface IUiDispatcher
{
    void Post(Action action);
}
