namespace Ambient.App.Tests;

/// <summary>Real-model engines contend for the one GPU; these run alone.</summary>
[CollectionDefinition("engine", DisableParallelization = true)]
public class EngineSuite
{
}
