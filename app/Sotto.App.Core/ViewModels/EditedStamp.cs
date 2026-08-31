using System.Globalization;

namespace Sotto.App.Core.ViewModels;

/// <summary>
/// "Edited 10:31" when the edit fell on the consultation's own day,
/// "Edited 27 Aug" otherwise, "" when never edited.
/// </summary>
public static class EditedStamp
{
    public static string Label(string sessionDayAt, string editedAt)
    {
        if (!DateTimeOffset.TryParse(editedAt, CultureInfo.InvariantCulture, out var edited))
        {
            return "";
        }

        var local = edited.ToLocalTime();
        var sameDay =
            DateTimeOffset.TryParse(sessionDayAt, CultureInfo.InvariantCulture, out var day)
            && day.ToLocalTime().Date == local.Date;
        return "Edited " + (sameDay
            ? local.ToString("HH:mm", CultureInfo.CurrentCulture)
            : local.ToString("d MMM", CultureInfo.CurrentCulture));
    }

    public static string Now() =>
        "Edited " + DateTimeOffset.Now.ToString("HH:mm", CultureInfo.CurrentCulture);
}
