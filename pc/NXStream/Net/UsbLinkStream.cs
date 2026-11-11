using System.Runtime.InteropServices;
using NXStream.Core;
using NXStream.Protocol;

namespace NXStream.Net;

public sealed class UsbLinkStream : ILinkStream
{
    private const int ChunkSize = 64 * 1024;
    private const int RingSize = 4 * 1024 * 1024;
    private const uint ReadTimeoutMs = 100;
    private const uint WriteTimeoutMs = 500;

    private readonly object _ring = new();
    private readonly object _writeGate = new();
    private readonly byte[] _data = new byte[RingSize];
    private readonly Thread _reader;
    private IntPtr _handle;
    private readonly int _interface;
    private readonly byte _endpointIn, _endpointOut;
    private int _head, _count;
    private volatile bool _closed;
    private UsbException? _error;

    public string Name => "USB";

    private UsbLinkStream(IntPtr handle, int iface, byte endpointIn, byte endpointOut)
    {
        _handle = handle;
        _interface = iface;
        _endpointIn = endpointIn;
        _endpointOut = endpointOut;
        _reader = new Thread(ReadLoop) { IsBackground = true, Name = "NXStream usb reader", Priority = ThreadPriority.AboveNormal };
        _reader.Start();
    }

    public static UsbLinkStream Open() => Open(Wire.UsbProductId, Wire.UsbProductName);

    public static UsbLinkStream Open(ushort productId, string productName)
    {
        UsbException? last = null;
        var devices = LibUsb.Find(Wire.UsbVendorId, productId);
        try
        {
            foreach (var device in devices)
            {
                var rc = UsbProbe.OpenConsole(device, productName, out var handle);
                if (rc == UsbProbe.Foreign) continue;
                if (rc < 0)
                {
                    last = new UsbException("open", rc);
                    continue;
                }
                if (!LibUsb.FindBulkInterface(device.Device, out var iface, out var epIn, out var epOut))
                {
                    iface = Wire.UsbInterface;
                    epIn = Wire.UsbEndpointIn;
                    epOut = Wire.UsbEndpointOut;
                    Log.Write("usb", "descriptor unreadable, using default interface");
                }
                var detach = LibUsb.AutoDetachKernelDriver(handle);
                if (detach < 0 && detach != LibUsb.NotSupported) Log.Write("usb", $"auto detach: {LibUsb.ErrorName(detach)}");
                var claim = LibUsb.ClaimInterface(handle, iface);
                if (claim < 0)
                {
                    LibUsb.Close(handle);
                    last = new UsbException("claim", claim);
                    continue;
                }
                LibUsb.ClearHalt(handle, epIn);
                LibUsb.ClearHalt(handle, epOut);
                Log.Write("usb", $"interface {iface}, in 0x{epIn:x2}, out 0x{epOut:x2}");
                return new UsbLinkStream(handle, iface, epIn, epOut);
            }
        }
        finally
        {
            foreach (var device in devices) device.Dispose();
        }
        throw last ?? new UsbException("find", LibUsb.NoDevice);
    }

    private void ReadLoop()
    {
        var chunk = new byte[ChunkSize];
        long total = 0;
        try
        {
            while (!_closed)
            {
                lock (_ring)
                {
                    while (!_closed && _count + ChunkSize > RingSize) Monitor.Wait(_ring, 100);
                    if (_closed) break;
                }
                var rc = LibUsb.Bulk(_handle, _endpointIn, ref chunk[0], ChunkSize, out var n, ReadTimeoutMs);
                if (rc < 0 && rc != LibUsb.Timeout)
                {
                    if (!_closed)
                    {
                        Log.Write("usb", $"read failed: {LibUsb.ErrorName(rc)}");
                        lock (_ring)
                        {
                            _error = new UsbException("read", rc);
                            Monitor.PulseAll(_ring);
                        }
                    }
                    break;
                }
                if (n <= 0) continue;
                total += n;
                lock (_ring)
                {
                    var tail = (_head + _count) % RingSize;
                    var first = Math.Min(n, RingSize - tail);
                    Buffer.BlockCopy(chunk, 0, _data, tail, first);
                    if (first < n) Buffer.BlockCopy(chunk, first, _data, 0, n - first);
                    _count += n;
                    Monitor.PulseAll(_ring);
                }
            }
        }
        finally
        {
            lock (_ring)
            {
                _closed = true;
                Monitor.PulseAll(_ring);
            }
            lock (_writeGate)
            {
                if (_handle != IntPtr.Zero)
                {
                    LibUsb.ReleaseInterface(_handle, _interface);
                    LibUsb.Close(_handle);
                    _handle = IntPtr.Zero;
                }
            }
            Log.Write("usb", $"closed, {total / 1048576.0:0.0} MB read");
        }
    }

    public int Read(Span<byte> buffer)
    {
        if (buffer.IsEmpty) return 0;
        lock (_ring)
        {
            while (_count == 0)
            {
                if (_error is not null) throw _error;
                if (_closed) return 0;
                Monitor.Wait(_ring);
            }
            var n = Math.Min(buffer.Length, _count);
            var first = Math.Min(n, RingSize - _head);
            _data.AsSpan(_head, first).CopyTo(buffer);
            if (first < n) _data.AsSpan(0, n - first).CopyTo(buffer[first..]);
            _head = (_head + n) % RingSize;
            _count -= n;
            Monitor.PulseAll(_ring);
            return n;
        }
    }

    public void Write(ReadOnlySpan<byte> data)
    {
        lock (_writeGate)
        {
            while (!data.IsEmpty)
            {
                if (_closed || _handle == IntPtr.Zero) throw new ObjectDisposedException(nameof(UsbLinkStream));
                var length = Math.Min(data.Length, ChunkSize);
                var rc = LibUsb.Bulk(_handle, _endpointOut, ref MemoryMarshal.GetReference(data), length,
                                     out var n, WriteTimeoutMs);
                if (rc < 0 && rc != LibUsb.Timeout) throw new UsbException("write", rc);
                data = data[Math.Max(0, n)..];
            }
        }
    }

    public void Close()
    {
        lock (_ring)
        {
            _closed = true;
            Monitor.PulseAll(_ring);
        }
    }

    public void Dispose() => Close();
}
