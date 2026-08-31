namespace Sotto.App.Core.ViewModels;

/// <summary>
/// The stages of a stop, in order. The engine reports Transcript and
/// Speakers as it starts them; the shell sets Note once the sealed transcript
/// has loaded and Streaming on the first note token.
/// </summary>
public enum FinalisePhase
{
    None,
    Sealing,
    Transcript,
    Speakers,
    Turns,  // the per-turn re-decode: transcript work again, long on the NPU
    Note,
    Streaming,
}
