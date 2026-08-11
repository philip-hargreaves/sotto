namespace Sotto.App.Core.Hosting;

/// <summary>
/// What recovery needs to know about the session: whether an engine death
/// would interrupt a consultation in progress.
/// </summary>
public interface ISessionState
{
    bool ConsultationActive { get; }
}
