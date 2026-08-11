using Sotto.App.Core;
using Sotto.App.Core.ViewModels;

namespace Sotto.App.Tests;

/// <summary>Runs posted work immediately; tests have no UI thread.</summary>
internal sealed class InlineDispatcher : IUiDispatcher
{
    public void Post(Action action) => action();
}

/// <summary>Records navigation calls without a real content host.</summary>
internal sealed class RecordingNavigationService : INavigationService
{
    private readonly Stack<string> _back = new();

    public string? Current { get; private set; }

    public bool CanGoBack => _back.Count > 0;

    public void NavigateTo(string pageKey)
    {
        if (Current is not null)
        {
            _back.Push(Current);
        }

        Current = pageKey;
    }

    public void GoBack() => Current = _back.Count > 0 ? _back.Pop() : Current;
}

internal static class TestSession
{
    public static (ConsultationViewModel Session, FakeEngineClient Engine, NoteViewModel Note)
        Create()
    {
        var engine = new FakeEngineClient(autoNotify: false);
        var note = new NoteViewModel();
        var session = new ConsultationViewModel(
            engine, new InlineDispatcher(), new TranscriptViewModel(), note,
            new StatusBarViewModel());
        return (session, engine, note);
    }
}
