using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text.Json;

namespace NXStream.Core;

public sealed record UpdateFile(string Name, string Url, string Sha256, long Size);

public sealed record UpdateComponent(string Id, string Version, IReadOnlyList<UpdateFile> Files);

public sealed record UpdateOffer(string Version, UpdateFile File);

public sealed class UpdateUrlPolicy
{
    private const string ReleasePath = "/burlone0/NXStream/releases/download/";
    private static readonly string[] LoopbackHosts = ["127.0.0.1", "::1", "localhost"];

    private readonly Uri? _origin;

    private UpdateUrlPolicy(Uri? origin) => _origin = origin;

    public static UpdateUrlPolicy Production { get; } = new(null);

    public bool IsOverride => _origin != null;

    public static bool TryForOverride(string baseUrl, out UpdateUrlPolicy policy)
    {
        policy = Production;
        if (!Uri.TryCreate(baseUrl, UriKind.Absolute, out var uri)) return false;
        if (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps) return false;
        if (!string.IsNullOrEmpty(uri.UserInfo)) return false;
        if (Array.IndexOf(LoopbackHosts, uri.Host.Trim('[', ']').ToLowerInvariant()) < 0) return false;
        policy = new UpdateUrlPolicy(uri);
        return true;
    }

    public bool IsHopAllowed(Uri uri)
    {
        if (!uri.IsAbsoluteUri || !string.IsNullOrEmpty(uri.UserInfo)) return false;
        if (_origin != null) return SameOrigin(uri, _origin);
        if (uri.Scheme != Uri.UriSchemeHttps || !uri.IsDefaultPort) return false;
        var host = uri.Host.ToLowerInvariant();
        return host == "github.com" || host.EndsWith(".githubusercontent.com", StringComparison.Ordinal);
    }

    public bool IsAssetUrl(string raw)
    {
        if (raw.Contains("..", StringComparison.Ordinal) || raw.Contains('\\')
            || raw.Contains("%2e", StringComparison.OrdinalIgnoreCase)) return false;
        if (!Uri.TryCreate(raw, UriKind.Absolute, out var uri)) return false;
        if (!string.IsNullOrEmpty(uri.UserInfo)) return false;
        if (_origin != null) return SameOrigin(uri, _origin);
        return uri.Scheme == Uri.UriSchemeHttps && uri.IsDefaultPort
            && string.Equals(uri.Host, "github.com", StringComparison.OrdinalIgnoreCase)
            && uri.AbsolutePath.StartsWith(ReleasePath, StringComparison.Ordinal);
    }

    private static bool SameOrigin(Uri a, Uri b) =>
        a.Scheme == b.Scheme && string.Equals(a.Host, b.Host, StringComparison.OrdinalIgnoreCase) && a.Port == b.Port;
}

public sealed class UpdateManifest
{
    private static readonly Dictionary<string, string> ExpectedFile = new()
    {
        ["windows"] = "NXStream.exe",
        ["linux"] = "NXStream.AppImage",
    };

    public int Schema { get; init; }
    public string Release { get; init; } = "";
    public IReadOnlyDictionary<string, UpdateComponent> Components { get; init; } = new Dictionary<string, UpdateComponent>();

    public static UpdateManifest? Parse(string json, UpdateUrlPolicy policy)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object) return null;
            if (!root.TryGetProperty("schema", out var schema) || schema.ValueKind != JsonValueKind.Number
                || !schema.TryGetInt32(out var schemaValue) || schemaValue != 1) return null;
            var release = Str(root, "release");
            if (release == null) return null;
            if (!root.TryGetProperty("components", out var comps) || comps.ValueKind != JsonValueKind.Object) return null;

            var result = new Dictionary<string, UpdateComponent>();
            foreach (var prop in comps.EnumerateObject())
            {
                var c = prop.Value;
                if (c.ValueKind != JsonValueKind.Object) return null;
                var id = Str(c, "id");
                var version = Str(c, "version");
                if (id == null || version == null) return null;
                if (!c.TryGetProperty("files", out var files) || files.ValueKind != JsonValueKind.Array) return null;
                var list = new List<UpdateFile>();
                foreach (var f in files.EnumerateArray())
                {
                    if (f.ValueKind != JsonValueKind.Object) return null;
                    var name = Str(f, "name");
                    var url = Str(f, "url");
                    var sha = Str(f, "sha256");
                    if (name == null || url == null || sha == null) return null;
                    if (!f.TryGetProperty("size", out var size) || size.ValueKind != JsonValueKind.Number
                        || !size.TryGetInt64(out var sizeValue)) return null;
                    if (!policy.IsAssetUrl(url)) return null;
                    list.Add(new UpdateFile(name, url, sha, sizeValue));
                }
                if (!ExpectedFile.TryGetValue(prop.Name, out var expected)) continue;
                if (list.Count != 1 || list[0].Name != expected) continue;
                result[prop.Name] = new UpdateComponent(id, version, list);
            }
            return new UpdateManifest { Schema = schemaValue, Release = release, Components = result };
        }
        catch (JsonException)
        {
            return null;
        }
    }

    public static UpdateOffer? Decide(string? localId, string localVersion, UpdateComponent? remote)
    {
        if (string.IsNullOrEmpty(localId) || localId == "dev") return null;
        if (remote == null || remote.Files.Count == 0) return null;
        if (remote.Id == localId) return null;
        if (CompareVersion(localVersion, remote.Version) >= 0) return null;
        return new UpdateOffer(remote.Version, remote.Files[0]);
    }

    public static int CompareVersion(string a, string b)
    {
        var pa = ParseVersion(a);
        var pb = ParseVersion(b);
        for (var i = 0; i < 3; i++)
        {
            var c = pa[i].CompareTo(pb[i]);
            if (c != 0) return c < 0 ? -1 : 1;
        }
        return 0;
    }

    private static long[] ParseVersion(string v)
    {
        var parts = new long[3];
        var s = v.Trim().TrimStart('v', 'V');
        var cut = s.IndexOfAny(new[] { '-', '+' });
        if (cut >= 0) s = s[..cut];
        var items = s.Split('.');
        if (items.Length != 3) return parts;
        var parsed = new long[3];
        for (var i = 0; i < 3; i++)
        {
            if (!long.TryParse(items[i], NumberStyles.None, CultureInfo.InvariantCulture, out parsed[i])) return parts;
        }
        return parsed;
    }

    private static string? Str(JsonElement e, string name)
    {
        if (!e.TryGetProperty(name, out var p) || p.ValueKind != JsonValueKind.String) return null;
        var s = p.GetString();
        return string.IsNullOrEmpty(s) ? null : s;
    }
}
