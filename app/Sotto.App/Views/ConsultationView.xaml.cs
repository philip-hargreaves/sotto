using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Sotto.App.Core.ViewModels;

namespace Sotto.App.Views;

public sealed partial class ConsultationView : UserControl
{
    public ConsultationView(
        ShellViewModel shell, SessionControlsView controls, TranscriptPaneView transcript,
        NotePaneView note, StatusBarView status, DemoTrayView demoTray, SettingsViewModel settings,
        MicViewModel mic)
    {
        Shell = shell;
        Controls = controls.ViewModel;
        Mic = mic;
        InitializeComponent();

        // The list is refreshed as the flyout opens: a headset plugged in a
        // moment ago is the normal case, not an edge one
        MicFlyout.Opening += async (_, _) =>
        {
            await Mic.RefreshAsync();
            BuildMicFlyout();
        };
        mic.PropertyChanged += (_, _) => Bindings.Update();

        ControlsHost.Content = controls;
        TranscriptHost.Content = transcript;
        NoteHost.Content = note;
        StatusHost.Content = status;
        DemoTrayHost.Content = demoTray;

        // The tray exists only while the settings toggle says so
        void Apply() => DemoTrayHost.Visibility =
            settings.DemoTrayEnabled ? Visibility.Visible : Visibility.Collapsed;
        Apply();
        settings.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(SettingsViewModel.DemoTrayEnabled))
            {
                Apply();
            }
        };
    }

    public ShellViewModel Shell { get; }

    public SessionControlsViewModel Controls { get; }

    public MicViewModel Mic { get; }

    public string MicTip => Controls.MicPickerEnabled
        ? Mic.FullName
        : "In use - changes apply to the next consultation";

    private void BuildMicFlyout()
    {
        MicFlyout.Items.Clear();
        if (!Mic.HasDevices)
        {
            MicFlyout.Items.Add(new MenuFlyoutItem
            {
                Text = "No microphone found - connect one to record",
                IsEnabled = false,
            });
            return;
        }

        foreach (var device in Mic.Devices)
        {
            var item = new RadioMenuFlyoutItem
            {
                Text = device.IsDefault ? $"{device.Name}  (default)" : device.Name,
                GroupName = "mic",
                IsChecked = device.Id == Mic.MicId,
            };
            var id = device.Id;
            item.Click += (_, _) => Mic.Select(id);
            MicFlyout.Items.Add(item);
            if (device.Bluetooth)
            {
                // The hands-free quality cliff, said where the choice is made
                MicFlyout.Items.Add(new MenuFlyoutItem
                {
                    Text = "    Bluetooth call mode - reduced recording quality",
                    IsEnabled = false,
                });
            }
        }
    }
}
