namespace Ambient.App.Core.Hosting;

/// <summary>
/// What recovery needs to know about the session: whether an engine death
/// would interrupt a consultation in progress.
/// </summary>
public interface ISessionState
{
    bool ConsultationActive { get; }

    /// <summary>Where the session was for the crash log; "" when unknown.</summary>
    string SessionPhase => "";
}
