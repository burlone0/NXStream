using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Text;
using NXStream.Core;
using NXStream.Protocol;

namespace NXStream.Net;

public static class NativeDiscovery
{
    public const string Label = "Nintendo Switch (Native)";

    private static readonly TimeSpan BeaconLifetime = TimeSpan.FromSeconds(7);
    private static readonly ConcurrentDictionary<IPAddress, (DateTime Seen, string? Name)> Beacons = new();
    private static volatile string? _knownName;
    private static UdpClient? _socket;

    public static void Forget() => _socket = null;

    public static void Remember(string name)
    {
        if (!string.IsNullOrWhiteSpace(name)) _knownName = name;
    }

    public static string? KnownName => _knownName;

    public static string? BeaconName(ReadOnlySpan<byte> data)
    {
        var text = Encoding.UTF8.GetString(data);
        var index = text.IndexOf(" name=", StringComparison.Ordinal);
        if (index < 0) return null;
        var name = new string(text[(index + 6)..].Where(c => !char.IsControl(c)).ToArray()).Trim();
        return name.Length == 0 ? null : name.Length > 64 ? name[..64] : name;
    }

    public static IReadOnlyList<SwitchDevice> Devices()
    {
        var now = DateTime.UtcNow;
        var list = new List<SwitchDevice>();
        foreach (var (address, beacon) in Beacons)
        {
            if (now - beacon.Seen > BeaconLifetime) Beacons.TryRemove(address, out _);
            else list.Add(new SwitchDevice(beacon.Name ?? _knownName ?? Label, address, Wire.NativeTcpPort, false));
        }
        return list.OrderBy(d => d.Address.ToString(), StringComparer.Ordinal).ToList();
    }

    public static IEnumerable<IPEndPoint> Recent(string name)
    {
        var devices = Devices();
        if (name != Label && name != _knownName && !devices.Any(d => d.Name == name)) return [];
        return devices.Where(d => name == Label || name == _knownName || d.Name == name).Select(d => new IPEndPoint(d.Address, d.Port)).ToList();
    }

    public static bool IsBeacon(ReadOnlySpan<byte> data) =>
        Encoding.ASCII.GetString(data).StartsWith(Wire.NativeBeaconMagic + " ", StringComparison.Ordinal);

    public static void Start(CancellationToken ct)
    {
        if (_socket is not null) return;
        UdpClient udp;
        try
        {
            udp = new UdpClient(AddressFamily.InterNetwork);
            udp.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
            udp.Client.Bind(new IPEndPoint(IPAddress.Any, Wire.NativeBeaconPort));
        }
        catch (SocketException ex)
        {
            Log.Write("disc", $"native beacon port {Wire.NativeBeaconPort} unavailable ({ex.SocketErrorCode})");
            return;
        }
        _socket = udp;
        Log.Write("disc", $"listening for native console beacons on udp {Wire.NativeBeaconPort}");
        _ = Task.Run(async () =>
        {
            try
            {
                while (!ct.IsCancellationRequested)
                {
                    try
                    {
                        var result = await udp.ReceiveAsync(ct);
                        if (!IsBeacon(result.Buffer)) continue;
                        if (!Beacons.ContainsKey(result.RemoteEndPoint.Address)) Log.Write("disc", "native console beacon seen");
                        Beacons[result.RemoteEndPoint.Address] = (DateTime.UtcNow, BeaconName(result.Buffer));
                    }
                    catch (Exception ex) when (ex is not (OperationCanceledException or OutOfMemoryException))
                    {
                        await Task.Delay(1000, ct);
                    }
                }
            }
            catch (OperationCanceledException) { }
            finally
            {
                udp.Dispose();
                if (ReferenceEquals(_socket, udp)) _socket = null;
            }
        });
    }
}
