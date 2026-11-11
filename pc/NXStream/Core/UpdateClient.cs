using System;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Security.Cryptography;
using System.Threading;
using System.Threading.Tasks;

namespace NXStream.Core;

public sealed class UpdateException(string reason) : Exception(reason);

public sealed class UpdateClient
{
    public const string DefaultBase = "https://github.com/burlone0/NXStream/releases/latest/download";
    private const string ManifestName = "nxstream-manifest.json";
    private const int MaxHops = 5;

    private readonly HttpClient _http;
    private readonly string _base;
    private readonly bool _overridden;
    private readonly UpdateUrlPolicy _policy;

    public UpdateClient(HttpClient? http = null, string? baseUrl = null)
    {
        _http = http ?? new HttpClient(new HttpClientHandler { AllowAutoRedirect = false });
        var requested = !string.IsNullOrWhiteSpace(baseUrl) ? baseUrl : Environment.GetEnvironmentVariable("NXSTREAM_UPDATE_BASE");
        _policy = UpdateUrlPolicy.Production;
        _base = DefaultBase;
        if (!string.IsNullOrWhiteSpace(requested))
        {
            if (UpdateUrlPolicy.TryForOverride(requested.TrimEnd('/'), out var policy))
            {
                _policy = policy;
                _base = requested.TrimEnd('/');
                _overridden = true;
            }
            else Log.Write("update", "base override ignored: not a loopback address");
        }
    }

    private async Task<HttpResponseMessage> GetAsync(string url, HttpCompletionOption option, CancellationToken ct)
    {
        var current = new Uri(url, UriKind.Absolute);
        for (var hop = 0; ; hop++)
        {
            if (!_policy.IsHopAllowed(current)) throw new UpdateException("url not allowed");
            using var req = new HttpRequestMessage(HttpMethod.Get, current);
            req.Headers.UserAgent.ParseAdd($"NXStream/{BuildInfo.Version}");
            var resp = await _http.SendAsync(req, option, ct).ConfigureAwait(false);
            var code = (int)resp.StatusCode;
            if (code is not (301 or 302 or 303 or 307 or 308)) return resp;
            var location = resp.Headers.Location;
            resp.Dispose();
            if (hop >= MaxHops) throw new UpdateException("too many redirects");
            if (location == null) throw new UpdateException("redirect without target");
            current = location.IsAbsoluteUri ? location : new Uri(current, location);
            Log.Write("update", $"redirect {hop + 1} status {code}");
        }
    }

    public async Task<UpdateOffer?> CheckAsync(CancellationToken ct)
    {
        Log.Write("update", $"check: overridden base={_overridden}");
        try
        {
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
            timeout.CancelAfter(TimeSpan.FromSeconds(8));
            using var resp = await GetAsync($"{_base}/{ManifestName}", HttpCompletionOption.ResponseContentRead, timeout.Token).ConfigureAwait(false);
            Log.Write("update", $"manifest http status {(int)resp.StatusCode}");
            if (resp.StatusCode != HttpStatusCode.OK) return null;
            var json = await resp.Content.ReadAsStringAsync(timeout.Token).ConfigureAwait(false);
            var manifest = UpdateManifest.Parse(json, _policy);
            if (manifest == null)
            {
                Log.Write("update", "manifest rejected (invalid or unexpected urls)");
                return null;
            }
            manifest.Components.TryGetValue(BuildInfo.Component, out var remote);
            var offer = UpdateManifest.Decide(BuildInfo.ContentId, BuildInfo.Version, remote);
            Log.Write("update", $"local id={BuildInfo.ContentId ?? "none"} version={BuildInfo.Version}, remote id={remote?.Id ?? "none"} version={remote?.Version ?? "none"}, verdict: {Reason(remote, offer)}");
            return offer;
        }
        catch (Exception ex)
        {
            Log.Write("update", $"check failed: {ex.GetType().Name}");
            return null;
        }
    }

    private static string Reason(UpdateComponent? remote, UpdateOffer? offer)
    {
        if (offer != null) return "update offered";
        if (string.IsNullOrEmpty(BuildInfo.ContentId) || BuildInfo.ContentId == "dev") return "no update, local build has no content id";
        if (remote == null) return "no update, component missing in manifest";
        if (remote.Id == BuildInfo.ContentId) return "no update, same content id";
        return "no update, remote version not newer";
    }

    public async Task DownloadAsync(UpdateOffer offer, string tmpPath, IProgress<int>? progress, CancellationToken ct)
    {
        var file = offer.File;
        bool mismatch;
        try
        {
            if (!_policy.IsAssetUrl(file.Url)) throw new UpdateException("asset url not allowed");
            using var resp = await GetAsync(file.Url, HttpCompletionOption.ResponseHeadersRead, ct).ConfigureAwait(false);
            Log.Write("update", $"download http status {(int)resp.StatusCode}");
            resp.EnsureSuccessStatusCode();
            using var hash = SHA256.Create();
            long total = 0;
            var last = -1;
            await using (var src = await resp.Content.ReadAsStreamAsync(ct).ConfigureAwait(false))
            await using (var dst = new FileStream(tmpPath, FileMode.Create, FileAccess.Write, FileShare.None, 81920, true))
            {
                var buf = new byte[81920];
                int n;
                while ((n = await src.ReadAsync(buf, ct).ConfigureAwait(false)) > 0)
                {
                    hash.TransformBlock(buf, 0, n, null, 0);
                    await dst.WriteAsync(buf.AsMemory(0, n), ct).ConfigureAwait(false);
                    total += n;
                    if (file.Size > 0)
                    {
                        var pct = (int)Math.Min(100, total * 100 / file.Size);
                        if (pct != last) { last = pct; progress?.Report(pct); }
                    }
                }
            }
            hash.TransformFinalBlock(Array.Empty<byte>(), 0, 0);
            var actual = Convert.ToHexString(hash.Hash!);
            mismatch = total != file.Size || !string.Equals(actual, file.Sha256, StringComparison.OrdinalIgnoreCase);
            Log.Write("update", $"download finished bytes={total} expected={file.Size} sha ok={!mismatch}");
        }
        catch (OperationCanceledException)
        {
            TryDelete(tmpPath);
            throw;
        }
        catch (Exception ex)
        {
            Log.Write("update", $"download failed: {ex.GetType().Name}");
            TryDelete(tmpPath);
            throw new UpdateException(UpdateText.ReasonDownload);
        }
        if (mismatch)
        {
            TryDelete(tmpPath);
            throw new UpdateException(UpdateText.ReasonChecksum);
        }
    }

    private static void TryDelete(string path)
    {
        try { File.Delete(path); } catch (IOException) { } catch (UnauthorizedAccessException) { }
    }
}
