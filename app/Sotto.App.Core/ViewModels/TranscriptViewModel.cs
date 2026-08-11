using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;

namespace Sotto.App.Core.ViewModels;

public sealed partial class TranscriptViewModel : ObservableObject
{
    public ObservableCollection<string> Turns { get; } = [];

    public void Clear() => Turns.Clear();
}
