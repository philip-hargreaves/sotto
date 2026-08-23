using System.Collections.ObjectModel;
using System.Globalization;
using CommunityToolkit.Mvvm.ComponentModel;

namespace Sotto.App.Core.ViewModels;

/// <summary>One transcript row; live turns carry no speaker until the seal.</summary>
public sealed record TranscriptTurnItem(string Speaker, string TimeLabel, string Text);

public sealed partial class TranscriptViewModel : ObservableObject
{
    private const int SampleRate = 16000;

    public ObservableCollection<TranscriptTurnItem> Turns { get; } = [];

    public void Add(string speaker, ulong firstFrame, string text) =>
        Turns.Add(new TranscriptTurnItem(speaker, TimeLabel(firstFrame), text));

    public void Clear() => Turns.Clear();

    private static string TimeLabel(ulong firstFrame) =>
        TimeSpan.FromSeconds(firstFrame / (double)SampleRate)
            .ToString(@"mm\:ss", CultureInfo.InvariantCulture);
}
