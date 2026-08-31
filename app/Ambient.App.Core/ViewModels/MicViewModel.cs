using System.Collections.ObjectModel;
using System.Text.Json;
using CommunityToolkit.Mvvm.ComponentModel;
using Ambient.Client;

namespace Ambient.App.Core.ViewModels;

/// <summary>A capture endpoint as the engine listed it; the id is WASAPI's.</summary>
public sealed record MicDevice(string Id, string Name, string ShortName, bool IsDefault, bool Bluetooth);

/// <summary>The microphone picker: the engine's list, fetched fresh per
/// open; the choice is stored by id and falls back only while gone.</summary>
public sealed partial class MicViewModel : ObservableObject
{
    private readonly IEngineClient _engine;
    private readonly AppPreferences? _preferences;

    public MicViewModel(IEngineClient engine, AppPreferences? preferences = null,
        IUiDispatcher? dispatcher = null)
    {
        _engine = engine;
        _preferences = preferences;
        SelectedId = preferences?.MicId ?? "";
        // Refresh at connect so the label is right before the first open
        if (dispatcher is not null)
        {
            engine.ConnectedChanged += connected => dispatcher.Post(() =>
            {
                if (connected)
                {
                    _ = RefreshAsync();
                }
            });
            if (engine.Connected)
            {
                _ = RefreshAsync();
            }
        }
    }

    public ObservableCollection<MicDevice> Devices { get; } = [];

    /// <summary>Empty means "the system default", which the engine pins.</summary>
    [ObservableProperty]
    public partial string SelectedId { get; private set; }

    public bool HasDevices => Devices.Count > 0;

    /// <summary>What session/start should pin: the choice, or default.</summary>
    public string MicId => Current?.Id ?? "";

    private MicDevice? Current =>
        Devices.FirstOrDefault(d => d.Id == SelectedId)
        ?? Devices.FirstOrDefault(d => d.IsDefault)
        ?? Devices.FirstOrDefault();

    /// <summary>Shortened only while still naming ONE device (several mics
    /// can all report "Microphone"): endpoint, +adapter, then full name.</summary>
    public string Label
    {
        get
        {
            if (Current is not { } current)
            {
                return "No microphone found";
            }

            var endpoint = Endpoint(current);
            if (Devices.Count(d => Endpoint(d) == endpoint) == 1)
            {
                return endpoint;
            }

            return Devices.Count(d => d.ShortName == current.ShortName) == 1
                ? current.ShortName
                : current.Name;
        }
    }

    /// <summary>The full name, so the trimmed label is never the only identification.</summary>
    public string FullName => Current?.Name ?? "No microphone found";

    private static string Endpoint(MicDevice device)
    {
        var cut = device.ShortName.IndexOf(" on ", StringComparison.OrdinalIgnoreCase);
        return cut > 0 ? device.ShortName[..cut] : device.ShortName;
    }

    /// <summary>Asks the engine what it can hear; called when the picker opens.</summary>
    public async Task RefreshAsync()
    {
        if (!_engine.Connected)
        {
            return;
        }

        try
        {
            var response = await _engine
                .RequestAsync("audio/inputs", null, TimeSpan.FromSeconds(5))
                .ConfigureAwait(true);
            Devices.Clear();
            foreach (var device in response.GetProperty("devices").EnumerateArray())
            {
                Devices.Add(new MicDevice(
                    device.GetProperty("id").GetString() ?? "",
                    device.GetProperty("name").GetString() ?? "Microphone",
                    device.GetProperty("shortName").GetString() ?? "Microphone",
                    device.GetProperty("isDefault").GetBoolean(),
                    device.GetProperty("bluetooth").GetBoolean()));
            }
        }
        catch (Exception)
        {
            // A failed refresh keeps the last list; the engine still resolves
        }

        Changed();
    }

    /// <summary>The clinician's pick, kept for every future consultation.</summary>
    public void Select(string id)
    {
        SelectedId = id;
        if (_preferences is not null)
        {
            _preferences.MicId = id;
            _preferences.Save();
        }

        Changed();
    }

    private void Changed()
    {
        OnPropertyChanged(nameof(Label));
        OnPropertyChanged(nameof(FullName));
        OnPropertyChanged(nameof(HasDevices));
        OnPropertyChanged(nameof(MicId));
    }
}
