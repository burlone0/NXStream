using System.Buffers.Binary;
using System.Text;

namespace NXStream.Protocol;

public static class Wire
{
    public const int TcpPort = 48500;
    public const int DiscoveryPort = 48501;
    public const int BeaconPort = 48502;
    public const uint Version = 2;

    public const string DiscoverMagic = "NSTREAM_DISCOVER_V2";
    public const string AnnounceMagic = "NSTREAM_HERE_V2";

    public const int HeaderSize = 8;
    public const byte FlagKeyframe = 0x01;
    public const byte FlagReplyRequested = 0x02;
    public const int MaxPayload = 4 * 1024 * 1024;

    public const ushort UsbVendorId = 0x18D1;
    public const ushort UsbProductId = 0x4EE0;
    public const string UsbProductName = "NXStream";
    public const byte UsbEndpointIn = 0x81;
    public const byte UsbEndpointOut = 0x01;
    public const int UsbInterface = 0;

    public const int NativeTcpPort = 9950;
    public const int NativeBeaconPort = 9951;
    public const ushort NativeUsbProductId = 0x4D00;
    public const string NativeUsbProductName = "NXStream Native";
    public const string NativeBeaconMagic = "SFTAP1";
    public const uint NativeMagic = 0x52544653;
    public const int NativeHeaderSize = 32;
    public const int NativeMaxPayload = 8 * 1024 * 1024;
    public const ushort NativeFlagVideo = 2;
    public const ushort NativeFlagKeyframe = 4;
    public const ushort NativeFlagAudio = 8;
    public const ushort NativeFlagStatus = 16;
    public const ushort NativeFlagInfo = 32;
    public const int NativeMaxInfoPayload = 1024;
    public const int NativeAudioRate = 48000;
    public const int NativeAudioChannels = 2;
    public static readonly byte[] NativeKeyframeRequest = "SFTK"u8.ToArray();
    public static readonly byte[] NativeBye = "SFTQ"u8.ToArray();
}

public enum MessageType : byte
{
    Hello = 0x01,
    Ping = 0x06,
    Bye = 0x07,
    KeyframeRequest = 0x08,
    ProbeAck = 0x09,

    Welcome = 0x80,
    Pong = 0x81,
    VideoH264 = 0x83,
    VideoJpeg = 0x84,
    Audio = 0x85,
    Probe = 0x87,
    AppInfo = 0x88,
}

public static class Messages
{
    private static byte[] Allocate(MessageType type, int payloadSize)
    {
        var msg = new byte[Wire.HeaderSize + payloadSize];
        msg[0] = (byte)type;
        BinaryPrimitives.WriteUInt32LittleEndian(msg.AsSpan(4), (uint)payloadSize);
        return msg;
    }

    public static byte[] Hello(string viewerName)
    {
        var name = Encoding.UTF8.GetBytes(viewerName.Length > 64 ? viewerName[..64] : viewerName);
        var msg = Allocate(MessageType.Hello, 4 + name.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(msg.AsSpan(Wire.HeaderSize), Wire.Version);
        name.CopyTo(msg.AsSpan(Wire.HeaderSize + 4));
        return msg;
    }

    public static byte[] Ping(long token)
    {
        var msg = Allocate(MessageType.Ping, 8);
        BinaryPrimitives.WriteInt64LittleEndian(msg.AsSpan(Wire.HeaderSize), token);
        return msg;
    }

    public static byte[] Bye() => Allocate(MessageType.Bye, 0);

    public static byte[] KeyframeRequest() => Allocate(MessageType.KeyframeRequest, 0);

    public static byte[] ProbeAck(uint sequence)
    {
        var msg = Allocate(MessageType.ProbeAck, 4);
        BinaryPrimitives.WriteUInt32LittleEndian(msg.AsSpan(Wire.HeaderSize), sequence);
        return msg;
    }
}
