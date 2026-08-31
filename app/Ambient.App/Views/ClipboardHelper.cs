using Ambient.App.Core.ViewModels;

namespace Ambient.App.Views;

internal static class ClipboardHelper
{
    // A DataPackage can be handed to SetContent only once, so retries need a
    // fresh one; a Flush refusal must not fail a SetContent that succeeded
    public static async Task CopyAsync(StatusBarViewModel status, string text, string what)
    {
        for (var attempt = 1; attempt <= 5; attempt++)
        {
            try
            {
                var data = new Windows.ApplicationModel.DataTransfer.DataPackage();
                data.SetText(text);
                Windows.ApplicationModel.DataTransfer.Clipboard.SetContent(data);
                try
                {
                    Windows.ApplicationModel.DataTransfer.Clipboard.Flush();
                }
                catch
                {
                }

                status.Append($"{what} copied");
                return;
            }
            catch (Exception)
            {
                if (attempt == 5)
                {
                    status.Append("Copy failed - the clipboard is unavailable");
                    return;
                }

                await Task.Delay(80 * attempt);
            }
        }
    }
}
