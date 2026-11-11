using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;
using NXStream.Net;

namespace NXStream.Core;

public sealed class DeviceWatcher
{
    private readonly Func<string?> _lastTarget;
    private readonly Func<bool> _canScan;
    private readonly Action<IReadOnlyList<SwitchDevice>> _changed;
    private string? _lastDiscovery;
    private string? _lastError;

    public DeviceWatcher(Func<string?> lastTarget, Func<bool> canScan, Action<IReadOnlyList<SwitchDevice>> changed)
    {
        _lastTarget = lastTarget;
        _canScan = canScan;
        _changed = changed;
    }

    public async Task RunAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            if (_canScan())
            {
                try
                {
                    IPAddress[] hints = IPAddress.TryParse(_lastTarget(), out var last) ? [last] : [];
                    var devices = await DiscoveryClient.DiscoverAsync(TimeSpan.FromMilliseconds(1200), ct, hints);
                    if (ct.IsCancellationRequested) return;
                    _lastError = null;
                    Report(devices);
                }
                catch (OperationCanceledException) { return; }
                catch (Exception ex) when (ex is not OutOfMemoryException)
                {
                    var error = ex is SocketException se ? $"socket {se.SocketErrorCode}" : ex.GetType().Name;
                    if (error != _lastError)
                    {
                        if (ex is SocketException) Log.Write("disc", $"discovery error {error}, retrying");
                        else Log.Error("disc", "discovery error, retrying", ex);
                    }
                    _lastError = error;
                }
            }
            try { await Task.Delay(TimeSpan.FromSeconds(3), ct); }
            catch (OperationCanceledException) { return; }
        }
    }

    private void Report(IReadOnlyList<SwitchDevice> devices)
    {
        var summary = string.Join(", ", devices.Select(d => $"'{d.Name}' {d.Address}:{d.Port}{(d.Busy ? " busy" : "")}"));
        if (summary == _lastDiscovery) return;
        Log.Write("disc", devices.Count == 0
            ? "no console found"
            : $"found {devices.Count} console(s), {devices.Count(d => d.Busy)} busy");
        _lastDiscovery = summary;
        _changed(devices);
    }
}
