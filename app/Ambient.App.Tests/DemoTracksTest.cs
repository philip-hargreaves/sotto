using Ambient.App.Core.Demo;

namespace Ambient.App.Tests;

public class DemoTracksTest
{
    [Fact]
    public void ManifestListsOnlyTracksWhoseWavsExist()
    {
        var root = Directory.CreateTempSubdirectory("ambient-demo-test");
        try
        {
            var wav = Path.Combine(root.FullName, "elbow.wav");
            File.WriteAllBytes(wav, new byte[44]);
            File.WriteAllText(Path.Combine(root.FullName, "tracks.json"), """
                {"tracks":[
                  {"name":"Elbow swelling","file":"elbow.wav"},
                  {"name":"Missing","file":"gone.wav"}
                ]}
                """);

            var tracks = DemoTracks.Parse(Path.Combine(root.FullName, "tracks.json"));

            var track = Assert.Single(tracks);
            Assert.Equal("Elbow swelling", track.Name);
            Assert.Equal(wav, track.Path);
        }
        finally
        {
            root.Delete(recursive: true);
        }
    }

    [Fact]
    public void ABrokenManifestYieldsNoTracks()
    {
        var root = Directory.CreateTempSubdirectory("ambient-demo-test");
        try
        {
            File.WriteAllText(Path.Combine(root.FullName, "tracks.json"), "not json");
            Assert.Empty(DemoTracks.Parse(Path.Combine(root.FullName, "tracks.json")));
        }
        finally
        {
            root.Delete(recursive: true);
        }
    }

    [Fact]
    public void DurationComesFromTheHeader()
    {
        var wav = SessionContractWav.Write(seconds: 3);
        try
        {
            Assert.Equal(3.0, DemoTracks.DurationSeconds(wav), 3);
        }
        finally
        {
            File.Delete(wav);
        }
    }

    [Fact]
    public void AnUnreadableFileHasZeroDuration()
    {
        Assert.Equal(0, DemoTracks.DurationSeconds("C:/does/not/exist.wav"));
    }
}
