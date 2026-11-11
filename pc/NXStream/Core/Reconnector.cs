using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;
using NXStream.Net;

namespace NXStream.Core;

public sealed class Reconnector
{
    private readonly string _consoleName;
    private readonly IPEndPoint _endpoint;
    private readonly Func<IPEndPoint, CancellationToken, Task<ViewerSession>> _connect;
    private readonly Action<ViewerSession, IPEndPoint> _connected;
    private readonly CancellationTokenSource _cts = new();

    public Reconnector(string consoleName, IPEndPoint endpoint, Func<IPEndPoint, CancellationToken, Task<ViewerSession>> connect,
                       Action<ViewerSession, IPEndPoint> connected)
    {
        _consoleName = consoleName;
        _endpoint = endpoint;
        _connect = connect;
        _connected = connected;
    }

    public void Start() => _ = RunAsync(_cts.Token);

    public void Cancel() => _cts.Cancel();

    private async Task RunAsync(CancellationToken ct)
    {
        var started = DateTime.UtcNow;
        for (var attempt = 1; !ct.IsCancellationRequested; attempt++)
        {
            foreach (var candidate in Candidates())
            {
                try
                {
                    var session = await _connect(candidate, ct);
                    if (ct.IsCancellationRequested)
                    {
                        session.Stop();
                        return;
                    }
                    Log.Write("ui", $"reconnected via {Source(candidate)} after {(DateTime.UtcNow - started).TotalSeconds:0} s " +
                                    $"({attempt} attempts)");
                    try
                    {
                        _connected(session, candidate);
                    }
                    catch (Exception ex) when (ex is not OutOfMemoryException)
                    {
                        session.Stop();
                        throw;
                    }
                    return;
                }
                catch (OperationCanceledException)
                {
                    return;
                }
                catch (Exception ex) when (ex is not OutOfMemoryException)
                {
                    if (attempt <= 3 || attempt % 15 == 0)
                        Log.Write("ui", $"reconnect attempt {attempt} via {Source(candidate)}: " +
                                        (ex is SocketException se ? $"socket {se.SocketErrorCode}" : ex is UsbException ue ? $"usb {ue.ErrorName}" : ex.GetType().Name));
                }
            }
            try { await Task.Delay(TimeSpan.FromSeconds(2), ct); }
            catch (OperationCanceledException) { return; }
        }
    }

    private string Source(IPEndPoint candidate) => UsbTarget.Is(candidate) ? "usb" : candidate.Equals(_endpoint) ? "last" : "beacon";

    private IEnumerable<IPEndPoint> Candidates()
    {
        yield return _endpoint;
        if (UsbTarget.Is(_endpoint)) yield break;
        foreach (var other in DiscoveryClient.RecentBeacons(_consoleName))
            if (!other.Equals(_endpoint)) yield return other;
    }
}
