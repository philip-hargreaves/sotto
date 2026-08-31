using Sotto.App.Core.ViewModels;

namespace Sotto.App.Views;

internal static class ClipboardHelper
{
    // A fresh DataPackage per attempt is required: a package can be handed to
    // SetContent only once, so reuse across retries throws and leaves the
    // clipboard empty. Flush only decides whether the content outlives the
    // process, so its refusal must not fail a SetContent that succeeded.
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
