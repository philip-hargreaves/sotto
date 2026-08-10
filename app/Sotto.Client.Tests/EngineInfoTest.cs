namespace Sotto.Client.Tests;

public class EngineInfoTest
{
    [Fact]
    public void NameIsSet() => Assert.Equal("sotto", EngineInfo.Name);

    [Fact]
    public void VersionMatchesEngineHeader() => Assert.Equal("0.1.0", EngineInfo.Version);
}
