using System.Net;
using System.Text;
using System.Text.Json;

namespace NXStream.Core;

public static class HomebrewIcons
{
    private const string CatalogUrl = "https://switch.cdn.fortheusers.org/repo.json";
    private const int MaxBytes = 8 * 1024 * 1024;
    private const long RetryMs = 10 * 60 * 1000;

    private static readonly object gate = new();
    private static readonly HttpClient http = new(new HttpClientHandler
    {
        AutomaticDecompression = DecompressionMethods.GZip | DecompressionMethods.Deflate
    })
    {
        Timeout = TimeSpan.FromSeconds(15)
    };

    private static IReadOnlyDictionary<string, string>? loaded;
    private static long nextTryAt;

    public static string Normalize(string s)
    {
        var sb = new StringBuilder(s.Length);
        foreach (var c in s)
        {
            if (c >= 'A' && c <= 'Z') sb.Append((char)(c + 32));
            else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) sb.Append(c);
        }
        return sb.ToString();
    }

    public static IReadOnlyDictionary<string, string> ParseCatalog(string json)
    {
        var result = new Dictionary<string, string>();
        try
        {
            using var doc = JsonDocument.Parse(json);
            if (doc.RootElement.ValueKind != JsonValueKind.Object
                || !doc.RootElement.TryGetProperty("packages", out var packages)
                || packages.ValueKind != JsonValueKind.Array)
                return result;
            var ambiguous = new HashSet<string>();
            var names = new List<(string Key, string Package)>();
            foreach (var p in packages.EnumerateArray())
            {
                if (p.ValueKind != JsonValueKind.Object) continue;
                var name = Str(p, "name");
                if (string.IsNullOrEmpty(name)) continue;
                var title = Str(p, "title");
                if (!string.IsNullOrEmpty(title))
                {
                    var key = Normalize(title);
                    if (key.Length > 0)
                    {
                        if (result.TryGetValue(key, out var existing))
                        {
                            if (existing != name) ambiguous.Add(key);
                        }
                        else
                        {
                            result[key] = name;
                        }
                    }
                }
                var nameKey = Normalize(name);
                if (nameKey.Length > 0) names.Add((nameKey, name));
            }
            foreach (var (key, package) in names)
            {
                if (!result.ContainsKey(key)) result[key] = package;
            }
            foreach (var key in ambiguous) result.Remove(key);
        }
        catch (JsonException)
        {
            result.Clear();
        }
        return result;
    }

    private static string? Str(JsonElement e, string key) =>
        e.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;

    public static string IconUrl(string packageName) =>
        "https://switch.cdn.fortheusers.org/packages/" + Uri.EscapeDataString(packageName) + "/icon.png";

    public static string? Find(IReadOnlyDictionary<string, string> catalog, string? appName)
    {
        if (string.IsNullOrEmpty(appName)) return null;
        var key = Normalize(appName);
        if (key.Length == 0) return null;
        return catalog.TryGetValue(key, out var package) ? IconUrl(package) : null;
    }

    public static string? Lookup(string? appName, CancellationToken ct)
    {
        if (string.IsNullOrEmpty(appName)) return null;
        var catalog = GetCatalog(ct);
        return catalog == null ? null : Find(catalog, appName);
    }

    private static IReadOnlyDictionary<string, string>? GetCatalog(CancellationToken ct)
    {
        lock (gate)
        {
            if (loaded != null) return loaded;
            if (Environment.TickCount64 < nextTryAt) return null;
            try
            {
                var json = Download(ct);
                var parsed = ParseCatalog(json);
                if (parsed.Count == 0) throw new InvalidDataException("empty catalog");
                loaded = parsed;
                Log.Write("discord", "homebrew catalog loaded, " + parsed.Count + " entries");
                return loaded;
            }
            catch (OperationCanceledException) when (ct.IsCancellationRequested)
            {
                return null;
            }
            catch (Exception ex)
            {
                nextTryAt = Environment.TickCount64 + RetryMs;
                var reason = ex is HttpRequestException h && h.StatusCode.HasValue
                    ? "http " + (int)h.StatusCode.Value
                    : ex.GetType().Name;
                Log.Write("discord", "homebrew catalog unavailable (" + reason + ")");
                return null;
            }
        }
    }

    private static string Download(CancellationToken ct)
    {
        using var response = http.Send(new HttpRequestMessage(HttpMethod.Get, CatalogUrl), HttpCompletionOption.ResponseHeadersRead, ct);
        response.EnsureSuccessStatusCode();
        if (response.Content.Headers.ContentLength > MaxBytes) throw new InvalidDataException("too large");
        using var stream = response.Content.ReadAsStream(ct);
        using var ms = new MemoryStream();
        var buffer = new byte[16384];
        int n;
        while ((n = stream.Read(buffer, 0, buffer.Length)) > 0)
        {
            ms.Write(buffer, 0, n);
            if (ms.Length > MaxBytes) throw new InvalidDataException("too large");
        }
        return Encoding.UTF8.GetString(ms.GetBuffer(), 0, (int)ms.Length);
    }
}
