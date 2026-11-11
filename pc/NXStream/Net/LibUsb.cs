using System.Runtime.InteropServices;

namespace NXStream.Net;

public sealed class UsbException : IOException
{
    public int Code { get; }
    public string ErrorName { get; }
    public bool NeedsDriver => Code is LibUsb.NotSupported or LibUsb.NotFound or LibUsb.Access;

    public UsbException(string what, int code) : base($"USB {what} failed: {LibUsb.ErrorName(code)}")
    {
        Code = code;
        ErrorName = LibUsb.ErrorName(code);
    }
}

internal sealed class UsbDeviceRef : IDisposable
{
    public IntPtr Device { get; }
    public int Bus { get; }
    public int Address { get; }
    public byte ProductIndex { get; }
    public byte SerialIndex { get; }

    public UsbDeviceRef(IntPtr device, int bus, int address, byte productIndex, byte serialIndex)
    {
        Device = device;
        Bus = bus;
        Address = address;
        ProductIndex = productIndex;
        SerialIndex = serialIndex;
    }

    public void Dispose() => LibUsb.UnrefDevice(Device);
}

internal static class LibUsb
{
    private const string Lib = "libusb-1.0";

    public const int Success = 0;
    public const int Io = -1;
    public const int Access = -3;
    public const int NoDevice = -4;
    public const int NotFound = -5;
    public const int Busy = -6;
    public const int Timeout = -7;
    public const int NotSupported = -12;

    [StructLayout(LayoutKind.Sequential)]
    private struct DeviceDescriptor
    {
        public byte Length, DescriptorType;
        public ushort BcdUsb;
        public byte DeviceClass, DeviceSubClass, DeviceProtocol, MaxPacketSize0;
        public ushort VendorId, ProductId, BcdDevice;
        public byte Manufacturer, Product, SerialNumber, NumConfigurations;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct EndpointDescriptor
    {
        public byte Length, DescriptorType, EndpointAddress, Attributes;
        public ushort MaxPacketSize;
        public byte Interval, Refresh, SynchAddress;
        public IntPtr Extra;
        public int ExtraLength;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct InterfaceDescriptor
    {
        public byte Length, DescriptorType, InterfaceNumber, AlternateSetting, NumEndpoints, InterfaceClass,
                    InterfaceSubClass, InterfaceProtocol, InterfaceString;
        public IntPtr Endpoint;
        public IntPtr Extra;
        public int ExtraLength;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct InterfaceEntry
    {
        public IntPtr AltSetting;
        public int NumAltSetting;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ConfigDescriptor
    {
        public byte Length, DescriptorType;
        public ushort TotalLength;
        public byte NumInterfaces, ConfigurationValue, ConfigurationString, Attributes, MaxPower;
        public IntPtr Interface;
        public IntPtr Extra;
        public int ExtraLength;
    }

    [DllImport(Lib)] private static extern int libusb_get_active_config_descriptor(IntPtr dev, out IntPtr config);
    [DllImport(Lib)] private static extern void libusb_free_config_descriptor(IntPtr config);
    [DllImport(Lib)] private static extern int libusb_set_auto_detach_kernel_driver(IntPtr handle, int enable);
    [DllImport(Lib)] private static extern int libusb_init(out IntPtr ctx);
    [DllImport(Lib)] private static extern nint libusb_get_device_list(IntPtr ctx, out IntPtr list);
    [DllImport(Lib)] private static extern void libusb_free_device_list(IntPtr list, int unrefDevices);
    [DllImport(Lib)] private static extern IntPtr libusb_ref_device(IntPtr dev);
    [DllImport(Lib)] private static extern void libusb_unref_device(IntPtr dev);
    [DllImport(Lib)] private static extern int libusb_get_device_descriptor(IntPtr dev, out DeviceDescriptor desc);
    [DllImport(Lib)] private static extern byte libusb_get_bus_number(IntPtr dev);
    [DllImport(Lib)] private static extern byte libusb_get_device_address(IntPtr dev);
    [DllImport(Lib)] private static extern int libusb_open(IntPtr dev, out IntPtr handle);
    [DllImport(Lib)] private static extern void libusb_close(IntPtr handle);
    [DllImport(Lib)] private static extern int libusb_get_string_descriptor_ascii(IntPtr handle, byte index, byte[] data, int length);
    [DllImport(Lib)] private static extern int libusb_claim_interface(IntPtr handle, int iface);
    [DllImport(Lib)] private static extern int libusb_release_interface(IntPtr handle, int iface);
    [DllImport(Lib)] private static extern int libusb_clear_halt(IntPtr handle, byte endpoint);
    [DllImport(Lib)] private static extern int libusb_bulk_transfer(IntPtr handle, byte endpoint, ref byte data, int length,
                                                                    out int transferred, uint timeoutMs);
    [DllImport(Lib)] private static extern IntPtr libusb_error_name(int code);

    private static readonly object ContextLock = new();
    private static IntPtr _context;

    private static IntPtr Context
    {
        get
        {
            lock (ContextLock)
            {
                if (_context != IntPtr.Zero) return _context;
                var rc = libusb_init(out var ctx);
                if (rc < 0) throw new UsbException("init", rc);
                _context = ctx;
                return ctx;
            }
        }
    }

    public static string ErrorName(int code)
    {
        try
        {
            var p = libusb_error_name(code);
            if (p != IntPtr.Zero) return Marshal.PtrToStringAnsi(p) ?? code.ToString();
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { }
        return code.ToString();
    }

    public static List<UsbDeviceRef> Find(ushort vendorId, ushort productId)
    {
        var found = new List<UsbDeviceRef>();
        var count = libusb_get_device_list(Context, out var list);
        if (count < 0) throw new UsbException("device list", (int)count);
        try
        {
            for (var i = 0; i < count; i++)
            {
                var dev = Marshal.ReadIntPtr(list, i * IntPtr.Size);
                if (libusb_get_device_descriptor(dev, out var desc) < 0) continue;
                if (desc.VendorId != vendorId || desc.ProductId != productId) continue;
                libusb_ref_device(dev);
                found.Add(new UsbDeviceRef(dev, libusb_get_bus_number(dev), libusb_get_device_address(dev), desc.Product, desc.SerialNumber));
            }
        }
        finally
        {
            libusb_free_device_list(list, 1);
        }
        return found;
    }

    public static void UnrefDevice(IntPtr dev) => libusb_unref_device(dev);

    public static int Open(IntPtr dev, out IntPtr handle) => libusb_open(dev, out handle);

    public static void Close(IntPtr handle) => libusb_close(handle);

    public static string? ProductString(IntPtr handle, byte index) => StringDescriptor(handle, index);

    public static string? StringDescriptor(IntPtr handle, byte index)
    {
        if (index == 0) return null;
        var buffer = new byte[128];
        var n = libusb_get_string_descriptor_ascii(handle, index, buffer, buffer.Length);
        return n > 0 ? System.Text.Encoding.ASCII.GetString(buffer, 0, n) : null;
    }

    public static int AutoDetachKernelDriver(IntPtr handle) => libusb_set_auto_detach_kernel_driver(handle, 1);

    public static bool FindBulkInterface(IntPtr dev, out int iface, out byte endpointIn, out byte endpointOut)
    {
        iface = 0;
        endpointIn = 0;
        endpointOut = 0;
        IntPtr config;
        try
        {
            if (libusb_get_active_config_descriptor(dev, out config) < 0 || config == IntPtr.Zero) return false;
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException)
        {
            return false;
        }
        try
        {
            var cfg = Marshal.PtrToStructure<ConfigDescriptor>(config);
            var entrySize = Marshal.SizeOf<InterfaceEntry>();
            var altSize = Marshal.SizeOf<InterfaceDescriptor>();
            var epSize = Marshal.SizeOf<EndpointDescriptor>();
            for (var i = 0; i < cfg.NumInterfaces; i++)
            {
                var entry = Marshal.PtrToStructure<InterfaceEntry>(cfg.Interface + i * entrySize);
                for (var a = 0; a < entry.NumAltSetting; a++)
                {
                    var alt = Marshal.PtrToStructure<InterfaceDescriptor>(entry.AltSetting + a * altSize);
                    if (alt.InterfaceClass != 0xFF) continue;
                    byte inAddr = 0, outAddr = 0;
                    for (var e = 0; e < alt.NumEndpoints; e++)
                    {
                        var ep = Marshal.PtrToStructure<EndpointDescriptor>(alt.Endpoint + e * epSize);
                        if ((ep.Attributes & 3) != 2) continue;
                        if ((ep.EndpointAddress & 0x80) != 0) { if (inAddr == 0) inAddr = ep.EndpointAddress; }
                        else if (outAddr == 0) outAddr = ep.EndpointAddress;
                    }
                    if (inAddr == 0 || outAddr == 0) continue;
                    iface = alt.InterfaceNumber;
                    endpointIn = inAddr;
                    endpointOut = outAddr;
                    return true;
                }
            }
            return false;
        }
        finally
        {
            libusb_free_config_descriptor(config);
        }
    }

    public static int ClaimInterface(IntPtr handle, int iface) => libusb_claim_interface(handle, iface);

    public static int ReleaseInterface(IntPtr handle, int iface) => libusb_release_interface(handle, iface);

    public static int ClearHalt(IntPtr handle, byte endpoint) => libusb_clear_halt(handle, endpoint);

    public static int Bulk(IntPtr handle, byte endpoint, ref byte data, int length, out int transferred, uint timeoutMs) =>
        libusb_bulk_transfer(handle, endpoint, ref data, length, out transferred, timeoutMs);
}
