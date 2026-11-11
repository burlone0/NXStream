using System.Net;

namespace NXStream.Net;

public static class UsbTarget
{
    public static IPAddress Address => IPAddress.None;

    public static IPEndPoint Endpoint => new(IPAddress.None, 0);

    public static IPEndPoint NativeEndpoint => new(IPAddress.None, 1);

    public static bool Is(IPAddress address) => address.Equals(IPAddress.None);

    public static bool Is(IPEndPoint endpoint) => endpoint.Port is 0 or 1 && Is(endpoint.Address);

    public static bool IsNative(IPEndPoint endpoint) => endpoint.Port == 1 && Is(endpoint.Address);
}
