using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Text;

namespace NXStream.Net;

public static class LocalNetwork
{
    public sealed record LanInterface(string Name, IPAddress Address, IPAddress Mask)
    {
        public IPAddress Broadcast
        {
            get
            {
                var ip = Address.GetAddressBytes();
                var mask = Mask.GetAddressBytes();
                var b = new byte[4];
                for (var i = 0; i < 4; i++) b[i] = (byte)(ip[i] | ~mask[i]);
                return new IPAddress(b);
            }
        }

        public IEnumerable<IPAddress> Hosts()
        {
            var ip = BitConverter.ToUInt32(Address.GetAddressBytes().Reverse().ToArray());
            var mask = BitConverter.ToUInt32(Mask.GetAddressBytes().Reverse().ToArray());
            var hostBits = 32 - System.Numerics.BitOperations.PopCount(mask);
            if (hostBits < 2 || hostBits > 10) yield break;
            var network = ip & mask;
            var count = (1u << hostBits) - 2;
            for (uint i = 1; i <= count; i++)
            {
                var host = network + i;
                if (host == ip) continue;
                yield return new IPAddress(BitConverter.GetBytes(host).Reverse().ToArray());
            }
        }

        public bool Contains(IPAddress target)
        {
            var ip = Address.GetAddressBytes();
            var mask = Mask.GetAddressBytes();
            var t = target.GetAddressBytes();
            for (var i = 0; i < 4; i++)
                if ((ip[i] & mask[i]) != (t[i] & mask[i])) return false;
            return true;
        }
    }

    private static readonly string[] VpnMarkers =
    [
        "nordlynx", "wireguard", "wintun", "tap-windows", "mullvad", "surfshark", "cyberghost", "windscribe",
        "private internet access", "hotspot shield",
        "fortinet", "cisco anyconnect", "globalprotect", "vpn", "warp", "cloudflare", "wgcf",
    ];

    private static readonly string[] LinuxTunnelPrefixes = ["wg", "tun", "tap"];

    public static IReadOnlyList<LanInterface> LanInterfaces()
    {
        var result = new List<LanInterface>();
        foreach (var nic in NetworkInterface.GetAllNetworkInterfaces())
        {
            if (nic.OperationalStatus != OperationalStatus.Up || IsVpn(nic) ||
                nic.NetworkInterfaceType is NetworkInterfaceType.Loopback or NetworkInterfaceType.Tunnel or NetworkInterfaceType.Ppp)
                continue;
            foreach (var ua in nic.GetIPProperties().UnicastAddresses)
            {
                if (ua.Address.AddressFamily != AddressFamily.InterNetwork || ua.IPv4Mask is null) continue;
                if (!IsPrivate(ua.Address) || HostBits(ua.IPv4Mask) < 2) continue;
                result.Add(new LanInterface(nic.Name, ua.Address, ua.IPv4Mask));
            }
        }
        return result;
    }

    public static LanInterface? LanFor(IPAddress target) =>
        LanInterfaces().FirstOrDefault(i => i.Contains(target));

    public static void BindToInterface(Socket socket, LanInterface lan)
    {
        if (!OperatingSystem.IsLinux()) return;
        try
        {
            socket.SetRawSocketOption(1, 25, Encoding.ASCII.GetBytes(lan.Name + "\0"));
            Core.Log.Write("net", "socket bound to its LAN interface");
        }
        catch (Exception ex)
        {
            var error = ex is SocketException se ? se.SocketErrorCode.ToString() : ex.GetType().Name;
            Core.Log.Write("net", $"bind to the LAN interface failed ({error}), using the routing table");
        }
    }

    public static IReadOnlyList<string> ActiveVpns() =>
        NetworkInterface.GetAllNetworkInterfaces()
            .Where(n => n.OperationalStatus == OperationalStatus.Up && IsVpn(n))
            .Select(n => n.Name)
            .ToList();

    public static void LogAdapters()
    {
        var nics = NetworkInterface.GetAllNetworkInterfaces();
        foreach (var lan in LanInterfaces())
        {
            var kind = nics.FirstOrDefault(n => n.Name == lan.Name)?.NetworkInterfaceType switch
            {
                NetworkInterfaceType.Wireless80211 => "wireless",
                NetworkInterfaceType.Ethernet or NetworkInterfaceType.GigabitEthernet => "ethernet",
                _ => "other",
            };
            Core.Log.Write("net", $"LAN adapter: {kind}, prefix /{32 - HostBits(lan.Mask)}");
        }
        Core.Log.Write("net", $"VPN present: {(ActiveVpns().Count > 0 ? "yes" : "no")}");
    }

    private static bool IsVpn(NetworkInterface nic)
    {
        var text = (nic.Name + " " + nic.Description).ToLowerInvariant();
        if (text.Contains("radmin") || text.Contains("zerotier") || text.Contains("hamachi")) return false;
        if (OperatingSystem.IsLinux() && LinuxTunnelPrefixes.Any(p => nic.Name.StartsWith(p, StringComparison.Ordinal))) return true;
        return VpnMarkers.Any(text.Contains);
    }

    private static int HostBits(IPAddress mask) =>
        32 - mask.GetAddressBytes().Sum(b => System.Numerics.BitOperations.PopCount(b));

    private static bool IsPrivate(IPAddress address)
    {
        var b = address.GetAddressBytes();
        return b[0] == 10 || (b[0] == 172 && b[1] is >= 16 and <= 31) || (b[0] == 192 && b[1] == 168);
    }
}
