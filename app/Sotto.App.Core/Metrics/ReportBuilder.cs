using System.Globalization;
using System.Net;
using System.Text;
using System.Text.Json;

namespace Sotto.App.Core.Metrics;

/// <summary>
/// Renders metrics.jsonl into one self-contained HTML file: readable in a
/// browser, with the raw JSON embedded for scripted aggregation.
/// </summary>
public static class ReportBuilder
{
    public static string Build(MachineInfo machine, IReadOnlyList<string> jsonlLines,
        DateTimeOffset exported)
    {
        var sessions = new List<JsonElement>();
        foreach (var line in jsonlLines)
        {
            try
            {
                sessions.Add(JsonDocument.Parse(line).RootElement.Clone());
            }
            catch (JsonException)
            {
            }
        }

        var html = new StringBuilder();
        html.Append("<!doctype html><html><head><meta charset=\"utf-8\">");
        html.Append("<title>Sotto Performance Report</title><style>");
        html.Append("body{font-family:Segoe UI,sans-serif;max-width:72rem;margin:2rem auto;");
        html.Append("padding:0 1rem;color:#1a1a1a}h1{font-size:1.5rem;margin-bottom:.2rem}");
        html.Append("h2{font-size:1.1rem;margin:2rem 0 .2rem}table{border-collapse:collapse;");
        html.Append("width:100%;font-size:.9rem;margin-top:.6rem}th,td{text-align:left;");
        html.Append("padding:.4rem .7rem;border-bottom:1px solid #ddd}th{color:#555;");
        html.Append("font-weight:600}.smoke{color:#aaa;text-decoration:line-through}");
        html.Append(".sub{color:#666;font-size:.9rem;margin:.1rem 0}pre{background:#f6f6f6;");
        html.Append("padding:1rem;overflow-x:auto;font-size:.8rem}</style></head><body>");
        html.Append("<h1>Sotto Performance Report</h1>");
        html.Append(CultureInfo.InvariantCulture,
            $"<p class=\"sub\">Exported {exported:yyyy-MM-dd HH:mm} UTC · "
            + $"{sessions.Count} recorded sessions</p>");

        AppendMachine(html, machine, sessions);
        AppendComparison(html, sessions);
        AppendSessions(html, sessions);

        html.Append("<h2>Raw Data</h2><details><summary>Session records (JSON)</summary><pre>");
        foreach (var session in sessions)
        {
            html.Append(WebUtility.HtmlEncode(session.GetRawText())).Append('\n');
        }

        html.Append("</pre></details>");
        html.Append("<script id=\"sotto-metrics\" type=\"application/json\">");
        html.Append(JsonSerializer.Serialize(new { machine, sessions }));
        html.Append("</script></body></html>");
        return html.ToString();
    }

    // Runtime-reported device names, with the matching Windows driver
    // version folded into the same row
    private static void AppendMachine(StringBuilder html, MachineInfo machine,
        List<JsonElement> sessions)
    {
        html.Append("<h2>Machine</h2><table>");
        Row(html, "Processor", $"{machine.Cpu} · {machine.RamGb} GB RAM");
        Row(html, "Windows", machine.Os);
        var drivers = machine.Gpus.ToList();
        if (machine.Npu is not null)
        {
            drivers.Add(machine.Npu);
        }

        var matched = new HashSet<GpuInfo>();
        var hardware = sessions.Select(s => Find(s, "engine", "hardware"))
            .LastOrDefault(h => h is { ValueKind: JsonValueKind.Object });
        if (hardware is not null)
        {
            foreach (var device in hardware.Value.EnumerateObject())
            {
                if (device.Name == "CPU")
                {
                    continue;
                }

                var name = device.Name == "NPU"
                    ? NpuGeneration(device.Value.GetString() ?? "")
                    : device.Value.GetString() ?? "";
                var driver = drivers.FirstOrDefault(d => SameDevice(d.Name, name));
                if (driver is not null)
                {
                    matched.Add(driver);
                }

                Row(html, device.Name,
                    driver is null ? name : $"{name} · driver {driver.Driver}");
            }
        }

        foreach (var driver in drivers.Where(d => !matched.Contains(d)))
        {
            Row(html, hardware is null ? "Device" : "Other device",
                $"{driver.Name} · driver {driver.Driver}");
        }

        var openvino = sessions.Select(s => Text(s, "engine", "openvino"))
            .LastOrDefault(v => v is not null);
        if (openvino is not null)
        {
            Row(html, "OpenVINO", openvino);
        }

        html.Append("</table>");
    }

    // The driver names every Intel NPU "AI Boost"; the architecture code is
    // the actual model, so translate it to the generation
    private static string NpuGeneration(string name)
    {
        var known = new Dictionary<string, string>
        {
            ["2700"] = "NPU 2",
            ["3720"] = "NPU 3",
            ["4000"] = "NPU 4",
            ["5000"] = "NPU 5",
        };
        foreach (var (arch, generation) in known)
        {
            if (name.Contains($"(arch {arch})", StringComparison.Ordinal))
            {
                return name.Replace($"(arch {arch})", $"· {generation} (arch {arch})");
            }
        }

        return name;
    }

    private static bool SameDevice(string a, string b)
    {
        static string Normalise(string name) => name
            .Replace("(R)", "").Replace("(TM)", "").Replace("  ", " ")
            .ToUpperInvariant().Trim();
        var (x, y) = (Normalise(a), Normalise(b));
        return x.Contains(y, StringComparison.Ordinal)
            || y.Contains(x, StringComparison.Ordinal)
            || (x.Contains("AI BOOST", StringComparison.Ordinal)
                && y.Contains("AI BOOST", StringComparison.Ordinal));
    }

    private static void AppendComparison(StringBuilder html, List<JsonElement> sessions)
    {
        var groups = sessions.Where(Comparable)
            .GroupBy(s => Text(s, "engine", "devices", "asr") ?? "?")
            .Where(g => g.Key != "?")
            .ToList();
        if (groups.Count < 2)
        {
            return;
        }

        html.Append("<h2>Transcription by Device</h2>");
        html.Append("<p class=\"sub\">Medians over microphone and real-time replay sessions; "
            + "accelerated test replays are excluded.</p><table><tr><th></th>");
        foreach (var group in groups)
        {
            html.Append(CultureInfo.InvariantCulture,
                $"<th>{WebUtility.HtmlEncode(group.Key)} ({group.Count()} sessions)</th>");
        }

        html.Append("</tr>");
        var rows = new (string Label, Func<JsonElement, double?> Value)[]
        {
            ("Transcription RTF", s => Number(s, "engine", "asrRealtimeFactor")),
            ("Transcript finalise (s)",
                s => Number(s, "engine", "stageSeconds", "transcript sealed")),
            ("Note prefill (s)", NotePrefill),
            ("Stop to note done (s)", s => Number(s, "note", "readyAfterStopSeconds")),
        };
        foreach (var (label, value) in rows)
        {
            html.Append(CultureInfo.InvariantCulture, $"<tr><td>{label}</td>");
            foreach (var group in groups)
            {
                html.Append(CultureInfo.InvariantCulture, $"<td>{Median(group, value)}</td>");
            }

            html.Append("</tr>");
        }

        html.Append("</table>");
    }

    private static void AppendSessions(StringBuilder html, List<JsonElement> sessions)
    {
        html.Append("<h2>Sessions</h2>");
        html.Append("<table><tr><th>Started</th><th>Recording</th><th>Source</th>")
            .Append("<th>Device</th><th>Transcription RTF</th><th>Audio length</th>")
            .Append("<th>Transcript finalise (s)</th><th>Note prefill (s)</th>")
            .Append("<th>Stop to note done (s)</th></tr>");
        foreach (var s in sessions)
        {
            html.Append(Comparable(s) ? "<tr>" : "<tr class=\"smoke\">");
            var speed = Number(s, "replaySpeed");
            Cell(html, Text(s, "start")?[..16].Replace('T', ' '));
            Cell(html, Text(s, "track") ?? "Microphone");
            Cell(html, speed is null ? "Microphone" : $"Replay ×{speed}");
            Cell(html, Text(s, "engine", "devices", "asr"));
            Cell(html, Format(Number(s, "engine", "asrRealtimeFactor")) + "×");
            Cell(html, Clock(Number(s, "engine", "audioSeconds")));
            Cell(html, Format(Number(s, "engine", "stageSeconds", "transcript sealed")));
            Cell(html, Format(NotePrefill(s)));
            Cell(html, Format(Number(s, "note", "readyAfterStopSeconds")));
            html.Append("</tr>");
        }

        html.Append("</table>");
    }

    // First words minus finalise and model load: the prompt-processing cost
    private static double? NotePrefill(JsonElement s)
    {
        var first = Number(s, "note", "firstPartialAfterStopSeconds");
        var sealedAt = Number(s, "engine", "stageSeconds", "transcript sealed");
        var load = Number(s, "engine", "loadSeconds", "note");
        if (first is null || sealedAt is null || load is null)
        {
            return null;
        }

        return Math.Max(0, Math.Round(first.Value - sealedAt.Value - load.Value, 1));
    }


    private static bool Comparable(JsonElement session)
    {
        var speed = Number(session, "replaySpeed");
        return speed is null || speed <= 1.0;
    }

    private static string Clock(double? seconds)
    {
        if (seconds is null)
        {
            return "-";
        }

        var whole = (int)Math.Round(seconds.Value);
        return $"{whole / 60}:{whole % 60:00}";
    }

    private static string Median(IEnumerable<JsonElement> sessions,
        Func<JsonElement, double?> value)
    {
        var values = sessions.Select(value).Where(v => v is not null)
            .Select(v => v!.Value).Order().ToList();
        return values.Count == 0 ? "-" : Format(values[values.Count / 2]);
    }

    private static string Format(double? value) => value is null
        ? "-"
        : value.Value.ToString(Math.Abs(value.Value) >= 100 ? "0" : "0.0",
            CultureInfo.InvariantCulture);

    private static void Row(StringBuilder html, string label, string value) =>
        html.Append(CultureInfo.InvariantCulture,
            $"<tr><th>{WebUtility.HtmlEncode(label)}</th>"
            + $"<td>{WebUtility.HtmlEncode(value)}</td></tr>");

    private static void Cell(StringBuilder html, string? value) =>
        html.Append(CultureInfo.InvariantCulture,
            $"<td>{WebUtility.HtmlEncode(value ?? "-")}</td>");

    private static JsonElement? Find(JsonElement root, params string[] path)
    {
        JsonElement current = root;
        foreach (var key in path)
        {
            if (current.ValueKind != JsonValueKind.Object
                || !current.TryGetProperty(key, out current))
            {
                return null;
            }
        }

        return current;
    }

    private static double? Number(JsonElement root, params string[] path)
    {
        var found = Find(root, path);
        return found is { ValueKind: JsonValueKind.Number } n ? n.GetDouble() : null;
    }

    private static string? Text(JsonElement root, params string[] path)
    {
        var found = Find(root, path);
        return found is { ValueKind: JsonValueKind.String } s ? s.GetString() : null;
    }
}
