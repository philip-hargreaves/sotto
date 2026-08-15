using System.Text.Json;

namespace Sotto.Contract.Tests;

/// <summary>
/// The engine/models method against the real engine.
/// </summary>
public class ModelsContractTest
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(10);

    [Fact]
    public async Task AnEmptyStoreListsNothing()
    {
        await using var engine = EngineProcess.Start($"LOCAL\\sotto-models-{Guid.NewGuid():N}");
        await using var client = await engine.ConnectAsync();

        var result = await client.RequestAsync("engine/models", null, Timeout);

        Assert.Equal(JsonValueKind.Array, result.GetProperty("models").ValueKind);
        Assert.Equal(0, result.GetProperty("models").GetArrayLength());
    }

    [Fact]
    public async Task AStagedManifestIsListedWithItsRole()
    {
        var modelsRoot = Path.Combine(Path.GetTempPath(), $"sotto-models-{Guid.NewGuid():N}");
        Directory.CreateDirectory(Path.Combine(modelsRoot, "silero-vad"));
        await File.WriteAllTextAsync(
            Path.Combine(modelsRoot, "silero-vad", "manifest.json"),
            """
            {"manifestVersion": 1, "id": "silero-vad", "task": "vad", "tier": "default",
             "licence": "MIT", "runtime": {"device": "CPU"}, "files": {"model.onnx": "00"}}
            """);
        try
        {
            await using var engine = EngineProcess.Start(
                $"LOCAL\\sotto-models-{Guid.NewGuid():N}", null, modelsRoot);
            await using var client = await engine.ConnectAsync();

            var result = await client.RequestAsync("engine/models", null, Timeout);

            var models = result.GetProperty("models");
            Assert.Equal(1, models.GetArrayLength());
            Assert.Equal("silero-vad", models[0].GetProperty("id").GetString());
            Assert.Equal("vad", models[0].GetProperty("task").GetString());
            Assert.Equal("default", models[0].GetProperty("tier").GetString());
            Assert.Equal("CPU", models[0].GetProperty("device").GetString());
        }
        finally
        {
            Directory.Delete(modelsRoot, recursive: true);
        }
    }
}
