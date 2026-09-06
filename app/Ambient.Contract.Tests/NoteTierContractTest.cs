using Ambient.Client;

namespace Ambient.Contract.Tests;

/// <summary>
/// The note/tier method against the real engine, with no weights staged:
/// the parameter checks and the lane-absent answer are the engine's own.
/// </summary>
public class NoteTierContractTest
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(10);

    [Fact]
    public async Task ATierThatIsNotOneIsAParameterError()
    {
        await using var engine = EngineProcess.Start($"LOCAL\\ambient-tier-{Guid.NewGuid():N}");
        await using var client = await engine.ConnectAsync();

        var error = await Assert.ThrowsAsync<EngineErrorException>(
            () => client.RequestAsync("note/tier", new { tier = "premium" }, Timeout));

        Assert.Equal(-32602, error.Code);
    }

    [Fact]
    public async Task WithNoNoteModelStagedTheLaneIsAbsentAndSaysSo()
    {
        await using var engine = EngineProcess.Start($"LOCAL\\ambient-tier-{Guid.NewGuid():N}");
        await using var client = await engine.ConnectAsync();

        var error = await Assert.ThrowsAsync<EngineErrorException>(
            () => client.RequestAsync("note/tier", new { tier = "default" }, Timeout));

        Assert.NotEqual(-32601, error.Code);  // the method exists
        Assert.Contains("no note model", error.ErrorData?.GetString() ?? "",
            StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task ModelsListSaysWhichModelEachRoleLoads()
    {
        var modelsRoot = Path.Combine(Path.GetTempPath(), $"ambient-models-{Guid.NewGuid():N}");
        foreach (var (id, tier) in new[] { ("qwen-small", "default"), ("qwen-large", "accuracy") })
        {
            Directory.CreateDirectory(Path.Combine(modelsRoot, id));
            await File.WriteAllTextAsync(
                Path.Combine(modelsRoot, id, "manifest.json"),
                $$$"""
                {"manifestVersion": 1, "id": "{{{id}}}", "task": "note", "tier": "{{{tier}}}",
                 "licence": "MIT", "runtime": {"device": "GPU"}, "files": {"model.xml": "00"}}
                """);
        }

        try
        {
            await using var engine = EngineProcess.Start(
                $"LOCAL\\ambient-tier-{Guid.NewGuid():N}", null, modelsRoot);
            await using var client = await engine.ConnectAsync();

            var result = await client.RequestAsync("engine/models", null, Timeout);

            foreach (var model in result.GetProperty("models").EnumerateArray())
            {
                var expected = model.GetProperty("tier").GetString() == "default";
                Assert.Equal(expected, model.GetProperty("active").GetBoolean());
            }
        }
        finally
        {
            Directory.Delete(modelsRoot, recursive: true);
        }
    }
}
