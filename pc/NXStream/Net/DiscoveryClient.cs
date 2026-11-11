using System.Net;
using System.Net.Sockets;
using System.Text;
using NXStream.Protocol;

namespace NXStream.Net;

public sealed record SwitchDevice(string Name, IPAddress Address, int Port, bool Busy);

public static class DiscoveryClient
{
    private static readonly TimeSpan BeaconLifetime = TimeSpan.FromSeconds(7);
    private static readonly System.Collections.Concurrent.ConcurrentDictionary<IPAddress, (SwitchDevice Device, DateTime Seen)> Beacons =
        new();
    private static UdpClient? _beaconSocket;

    public static void ForgetBeaconListener()
    {
        _beaconSocket = null;
        NativeDiscovery.Forget();
    }

    public static void StartBeaconListener(CancellationToken ct)
    {
        NativeDiscovery.Start(ct);
        if (_beaconSocket is not null) return;
        try
        {
            var udp = new UdpClient(AddressFamily.InterNetwork);
            udp.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
            udp.Client.Bind(new IPEndPoint(IPAddress.Any, Wire.BeaconPort));
            _beaconSocket = udp;
            Core.Log.Write("disc", $"listening for console beacons on udp {Wire.BeaconPort}");
        }
        catch (SocketException ex)
        {
            Core.Log.Write("disc", $"beacon port {Wire.BeaconPort} unavailable ({ex.SocketErrorCode}), probes only");
            return;
        }

        var listener = _beaconSocket!;
        _ = Task.Run(async () =>
        {
            string? lastError = null;
            try
            {
                while (!ct.IsCancellationRequested)
                {
                    try
                    {
                        var result = await listener.ReceiveAsync(ct);
                        lastError = null;
                        if (Parse(result.Buffer, result.RemoteEndPoint.Address) is { } device)
                        {
                            if (!Beacons.ContainsKey(device.Address))
                                Core.Log.Write("disc", $"console beacon seen: port {device.Port} busy {device.Busy}");
                            Beacons[device.Address] = (device, DateTime.UtcNow);
                        }
                    }
                    catch (Exception ex) when (ex is not (OperationCanceledException or OutOfMemoryException))
                    {
                        var error = ex is SocketException se ? $"socket {se.SocketErrorCode}" : ex.GetType().Name;
                        if (error != lastError)
                        {
                            if (ex is SocketException) Core.Log.Write("disc", $"beacon listener error {error}, retrying");
                            else Core.Log.Error("disc", "beacon listener error, retrying", ex);
                        }
                        lastError = error;
                        await Task.Delay(1000, ct);
                    }
                }
            }
            catch (OperationCanceledException) { }
            finally
            {
                listener.Dispose();
                if (ReferenceEquals(_beaconSocket, listener)) _beaconSocket = null;
            }
        });
    }

    public static async Task<IReadOnlyList<SwitchDevice>> DiscoverAsync(TimeSpan timeout, CancellationToken ct,
        IEnumerable<IPAddress>? hints = null)
    {
        var found = new System.Collections.Concurrent.ConcurrentDictionary<IPAddress, SwitchDevice>();
        using var cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        cts.CancelAfter(timeout);
        var hintList = hints?.ToList() ?? [];

        var sockets = new List<(UdpClient Udp, List<IPAddress> Targets)>();
        foreach (var lan in LocalNetwork.LanInterfaces())
        {
            try
            {
                var udp = CreateSocket(new IPEndPoint(lan.Address, 0), lan);
                var targets = hintList.Where(lan.Contains).ToList();
                targets.Add(lan.Broadcast);
                targets.Add(IPAddress.Broadcast);
                targets.AddRange(lan.Hosts());
                sockets.Add((udp, targets.Distinct().ToList()));
            }
            catch (SocketException) {  }
        }
        if (sockets.Count == 0)
            sockets.Add((CreateSocket(new IPEndPoint(IPAddress.Any, 0), null), [.. hintList, IPAddress.Broadcast]));

        try
        {
            var probe = Encoding.ASCII.GetBytes(Wire.DiscoverMagic);
            await Task.WhenAll(sockets.Select(s => ProbeAsync(s.Udp, s.Targets, probe, found, cts.Token)));
        }
        finally
        {
            foreach (var (udp, _) in sockets) udp.Dispose();
        }

        var now = DateTime.UtcNow;
        foreach (var (address, beacon) in Beacons)
        {
            if (now - beacon.Seen > BeaconLifetime) Beacons.TryRemove(address, out _);
            else found.TryAdd(address, beacon.Device);
        }
        return found.Values.Concat(NativeDiscovery.Devices()).OrderBy(d => d.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
    }

    public static IEnumerable<IPEndPoint> RecentBeacons(string name)
    {
        var now = DateTime.UtcNow;
        return Beacons.Values.Where(b => now - b.Seen < BeaconLifetime && b.Device.Name == name)
            .Select(b => new IPEndPoint(b.Device.Address, b.Device.Port)).Concat(NativeDiscovery.Recent(name)).ToList();
    }

    private static UdpClient CreateSocket(IPEndPoint local, LocalNetwork.LanInterface? lan)
    {
        var socket = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
        try
        {
            if (lan is not null) LocalNetwork.BindToInterface(socket, lan);
            socket.Bind(local);
        }
        catch
        {
            socket.Dispose();
            throw;
        }
        var udp = new UdpClient { Client = socket, EnableBroadcast = true };
        const int SioUdpConnReset = -1744830452;
        if (OperatingSystem.IsWindows())
        {
            try { udp.Client.IOControl(SioUdpConnReset, [0, 0, 0, 0], null); } catch (SocketException) {  }
        }
        return udp;
    }

    private static async Task ProbeAsync(UdpClient udp, List<IPAddress> targets, byte[] probe,
        System.Collections.Concurrent.ConcurrentDictionary<IPAddress, SwitchDevice> found, CancellationToken ct)
    {
        _ = Task.Run(async () =>
        {
            for (var round = 0; round < 2 && !ct.IsCancellationRequested; round++)
            {
                foreach (var target in targets)
                {
                    try { await udp.SendAsync(probe, probe.Length, new IPEndPoint(target, Wire.DiscoveryPort)); }
                    catch (Exception ex) when (ex is SocketException or ObjectDisposedException) {  }
                }
                try { await Task.Delay(300, ct); } catch (OperationCanceledException) { return; }
            }
        }, ct);

        try
        {
            while (true)
            {
                var result = await udp.ReceiveAsync(ct);
                if (Parse(result.Buffer, result.RemoteEndPoint.Address) is { } device) found[device.Address] = device;
            }
        }
        catch (OperationCanceledException) {  }
        catch (Exception ex) when (ex is SocketException or ObjectDisposedException) {  }
    }

    private static SwitchDevice? Parse(byte[] data, IPAddress from)
    {
        var lines = Encoding.UTF8.GetString(data).Split('\n');
        if (lines.Length < 3 || lines[0] != Wire.AnnounceMagic) return null;
        if (!int.TryParse(lines[2], out var port) || port is <= 0 or > 65535) port = Wire.TcpPort;
        var busy = lines.Length > 3 && lines[3].Trim() == "1";
        var name = new string(lines[1].Where(c => !char.IsControl(c)).ToArray()).Trim();
        if (name.Length == 0) name = "Nintendo Switch";
        else if (name.Length > 64) name = name[..64];
        return new SwitchDevice(name, from, port, busy);
    }
}
