using Sotto.App.Core;
using Sotto.App.Core.ViewModels;

namespace Sotto.App.Tests;

/// <summary>Runs posted work immediately; tests have no UI thread.</summary>
internal sealed class InlineDispatcher : IUiDispatcher
{
    public void Post(Action action) => action();
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
