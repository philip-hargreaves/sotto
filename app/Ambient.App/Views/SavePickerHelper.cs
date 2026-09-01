using Windows.Storage.Pickers;

namespace Ambient.App.Views;

/// <summary>One save picker for everything that writes outside the encrypted store.</summary>
internal static class SavePickerHelper
{
    public static async Task<string?> PickAsync(string suggestedName, string typeLabel,
        string extension)
    {
        var window = App.Current.Window;
        if (window is null)
        {
            return null;
        }

        var picker = new FileSavePicker
        {
            SuggestedFileName = suggestedName,
            SuggestedStartLocation = PickerLocationId.DocumentsLibrary,
        };
        picker.FileTypeChoices.Add(typeLabel, [extension]);
        // Unpackaged WinUI: the picker must be bound to our window handle
        WinRT.Interop.InitializeWithWindow.Initialize(
            picker, WinRT.Interop.WindowNative.GetWindowHandle(window));
        var file = await picker.PickSaveFileAsync();
        return file?.Path;
    }
}
