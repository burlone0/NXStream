using System.Linq;
using System.Net;

namespace NXStream.Core;

public static class EndpointParser
{
    public static bool TryParse(string text, out IPEndPoint endpoint)
    {
        endpoint = null!;
        var parts = text.Trim().Split(':');
        if (parts.Length > 2 || parts[0].Count(c => c == '.') != 3 || !IPAddress.TryParse(parts[0], out var ip)) return false;
        var port = Protocol.Wire.TcpPort;
        if (parts.Length == 2 && (!int.TryParse(parts[1], out port) || port is <= 0 or > 65535)) return false;
        endpoint = new IPEndPoint(ip, port);
        return true;
    }
}
