using System.Text.Json;
using Sotto.App.Core.ViewModels;
using Sotto.Client;

namespace Sotto.App.Tests;

public class SessionsViewModelTest
{
    private sealed class ScriptedEngineClient : IEngineClient
    {
        public Dictionary<string, object> Responses { get; } = [];

        public List<(string Method, string? Id)> Calls { get; } = [];

        public event Action<string, JsonElement>? NotificationReceived
        {
            add { }
            remove { }
        }

        public event Action<bool>? ConnectedChanged
        {
            add { }
            remove { }
        }

        public bool Connected => true;

        public Task<JsonElement> RequestAsync(
            string method, object? parameters, TimeSpan timeout,
            CancellationToken cancellationToken = default)
        {
            var id = parameters is null
                ? null
                : JsonSerializer.SerializeToElement(parameters).GetProperty("id").GetString();
            Calls.Add((method, id));
            if (!Responses.TryGetValue(method, out var response))
            {
                throw new InvalidOperationException($"no session {id}");
            }

            return Task.FromResult(JsonSerializer.SerializeToElement(response));
        }

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }

    private static ScriptedEngineClient EngineWithOneSession() => new()
    {
        Responses =
        {
            ["session/list"] = new
            {
                sessions = new[]
                {
                    new
                    {
                        id = "abc",
                        startedAt = "2026-08-17T10:15:00Z",
                        endedAt = "2026-08-17T10:23:41Z",
                        state = "finalised",
                        sampleRate = 16000,
                    },
                },
            },
            ["session/transcript"] = new
            {
                turns = new[]
                {
                    new { firstFrame = 480000L, frameCount = 48000L, speaker = "", text = "hello" },
                },
            },
            ["session/delete"] = new { },
        },
    };

    [Fact]
    public async Task RefreshListsSessionsWithFormattedFields()
    {
        var engine = EngineWithOneSession();
        var vm = new SessionsViewModel(engine, new StatusBarViewModel());

        await vm.RefreshAsync();

        var row = Assert.Single(vm.Sessions);
        Assert.Equal("abc", row.Id);
        Assert.Equal("08:41", row.Duration);
        Assert.Equal("finalised", row.State);
    }

    [Fact]
    public async Task SelectingASessionLoadsItsTurns()
    {
        var engine = EngineWithOneSession();
        var vm = new SessionsViewModel(engine, new StatusBarViewModel());
        await vm.RefreshAsync();

        vm.Selected = vm.Sessions[0];
        await Task.Delay(50);

        var turn = Assert.Single(vm.Turns);
        Assert.Equal("00:30", turn.Timestamp);
        Assert.Equal("hello", turn.Text);
        Assert.Contains(("session/transcript", "abc"), engine.Calls);
    }

    [Fact]
    public async Task DeselectingClearsTheTurns()
    {
        var engine = EngineWithOneSession();
        var vm = new SessionsViewModel(engine, new StatusBarViewModel());
        await vm.RefreshAsync();
        vm.Selected = vm.Sessions[0];
        await Task.Delay(50);

        vm.Selected = null;
        await Task.Delay(50);

        Assert.Empty(vm.Turns);
    }

    [Fact]
    public async Task DeleteCallsTheEngineAndRefreshes()
    {
        var engine = EngineWithOneSession();
        var vm = new SessionsViewModel(engine, new StatusBarViewModel());
        await vm.RefreshAsync();
        vm.Selected = vm.Sessions[0];

        await vm.DeleteSelectedAsync();

        Assert.Contains(("session/delete", "abc"), engine.Calls);
        Assert.Equal(2, engine.Calls.Count(c => c.Method == "session/list"));
    }

    [Fact]
    public async Task EngineErrorsLandInTheStatusLogNotAsCrashes()
    {
        var engine = new ScriptedEngineClient();  // every request throws
        var status = new StatusBarViewModel();
        var vm = new SessionsViewModel(engine, status);

        await vm.RefreshAsync();

        Assert.Empty(vm.Sessions);
        Assert.Contains(status.LogEntries, line => line.Contains("could not list sessions"));
    }

    [Fact]
    public async Task AnEmptyStoreIsAnEmptyListNotAnError()
    {
        var engine = new ScriptedEngineClient
        {
            Responses = { ["session/list"] = new { sessions = Array.Empty<object>() } },
        };
        var vm = new SessionsViewModel(engine, new StatusBarViewModel());

        await vm.RefreshAsync();

        Assert.Empty(vm.Sessions);
    }
}
