namespace Ambient.App.Core.ViewModels;

/// <summary>
/// Generation is sequential, so the pipeline has exactly these states; a
/// patient leaflet without a clinical note cannot be represented.
/// </summary>
public enum NotePipelineState
{
    Pending,
    NoteWriting,
    NoteReadyPatientWriting,
    AllReady,
    NoteFailed,
    PatientFailed,
    NoteRefused,
}

/// <summary>Engine-reported progress; the view model projects, never sequences.</summary>
public enum NotePipelineEvent
{
    NoteWritingStarted,
    NoteReady,
    NoteFailed,
    NoteRefused,
    PatientInfoReady,
    PatientInfoFailed,
}
