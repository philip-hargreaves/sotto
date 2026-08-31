using System.Collections.ObjectModel;
using System.Text.Json;
using CommunityToolkit.Mvvm.ComponentModel;
using Sotto.Client;

namespace Sotto.App.Core.ViewModels;

/// <summary>A capture endpoint as the engine listed it; the id is WASAPI's.</summary>
public sealed record MicDevice(string Id, string Name, string ShortName, bool IsDefault, bool Bluetooth);

/// <summary>
/// The microphone picker. The list is the engine's, fetched fresh each time
/// the flyout opens; the choice is stored by id and falls back to the system
/// default only when the chosen device is genuinely gone.
/// </summary>
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
        // The label must be right before the picker is ever opened
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

    /// <summary>
    /// Shortened only as far as it can be while still naming ONE device: a
    /// Bluetooth headset, a USB mic and the built-in array can all report an
    /// endpoint of "Microphone", and three identical labels would be worse
    /// than one long one. Windows names endpoints "endpoint on adapter", so
    /// the steps are endpoint alone, then with the adapter, then the full name.
    /// </summary>
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
