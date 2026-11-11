using NXStream.Core;
using NXStream.Protocol;

namespace NXStream.Net;

public sealed record UsbConsole(int Bus, int Address, bool Openable, int OpenError, bool Native = false, string? Name = null)
{
    public string ErrorName => OpenError == 0 ? "" : LibUsb.ErrorName(OpenError);
    public bool NeedsDriver => !Openable && OpenError is LibUsb.NotSupported or LibUsb.NotFound or LibUsb.Access;
}

internal static class UsbProbe
{
    public const int Foreign = 1;

    public static int OpenConsole(UsbDeviceRef device, string productName, out IntPtr handle)
    {
        var rc = LibUsb.Open(device.Device, out handle);
        if (rc < 0) return rc;
        if (LibUsb.ProductString(handle, device.ProductIndex) == productName) return 0;
        LibUsb.Close(handle);
        handle = IntPtr.Zero;
        return Foreign;
    }
}

public sealed class UsbDiscovery
{
    private readonly Func<bool> _canScan;
    private readonly SemaphoreSlim _wake = new(0);
    private readonly Dictionary<(int, int), UsbConsole> _known = new();
    private string? _lastSummary;
    private string? _lastError;

    public event Action<IReadOnlyList<UsbConsole>>? Changed;

    public bool LibraryMissing { get; private set; }

    public UsbDiscovery(Func<bool> canScan) => _canScan = canScan;

    public void Rescan()
    {
        if (_wake.CurrentCount == 0) _wake.Release();
    }

    public async Task RunAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            if (_canScan())
            {
                try
                {
                    var consoles = await Task.Run(Scan, ct);
                    _lastError = null;
                    Report(consoles);
                }
                catch (OperationCanceledException) { return; }
                catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException)
                {
                    LibraryMissing = true;
                    Log.Write("usb", $"libusb not available: {ex.GetType().Name}");
                    Changed?.Invoke([]);
                    return;
                }
                catch (UsbException ex)
                {
                    if (ex.ErrorName != _lastError) Log.Write("usb", $"scan failed: {ex.ErrorName}");
                    _lastError = ex.ErrorName;
                }
            }
            try { await _wake.WaitAsync(TimeSpan.FromSeconds(1), ct); }
            catch (OperationCanceledException) { return; }
        }
    }

    private List<UsbConsole> Scan()
    {
        var result = new List<UsbConsole>();
        var present = new HashSet<(int, int)>();
        ScanProduct(Wire.UsbProductId, Wire.UsbProductName, false, result, present);
        ScanProduct(Wire.NativeUsbProductId, Wire.NativeUsbProductName, true, result, present);
        foreach (var key in _known.Keys.Where(k => !present.Contains(k)).ToList()) _known.Remove(key);
        result.Sort((a, b) => (a.Bus, a.Address).CompareTo((b.Bus, b.Address)));
        return result;
    }

    private void ScanProduct(ushort productId, string productName, bool native, List<UsbConsole> result, HashSet<(int, int)> present)
    {
        var devices = LibUsb.Find(Wire.UsbVendorId, productId);
        try
        {
            foreach (var device in devices)
            {
                var key = (device.Bus, device.Address);
                present.Add(key);
                if (_known.TryGetValue(key, out var cached) && (cached.Openable || cached.OpenError == UsbProbe.Foreign))
                {
                    if (cached.OpenError != UsbProbe.Foreign) result.Add(cached);
                    continue;
                }
                var rc = UsbProbe.OpenConsole(device, productName, out var handle);
                string? name = null;
                if (rc == 0)
                {
                    if (!native)
                    {
                        try { name = LibUsb.StringDescriptor(handle, device.SerialIndex); }
                        catch (Exception ex) when (ex is not (DllNotFoundException or EntryPointNotFoundException or BadImageFormatException)) { }
                    }
                    LibUsb.Close(handle);
                }
                var console = new UsbConsole(device.Bus, device.Address, rc == 0, rc, native, name);
                _known[key] = console;
                if (rc != UsbProbe.Foreign) result.Add(console);
            }
        }
        finally
        {
            foreach (var device in devices) device.Dispose();
        }
    }

    private void Report(IReadOnlyList<UsbConsole> consoles)
    {
        var summary = string.Join(",", consoles.Select(c => $"{c.Bus}.{c.Address}:{c.OpenError}{(c.Native ? "n" : "")}"));
        if (summary == _lastSummary) return;
        _lastSummary = summary;
        if (consoles.Count == 0) Log.Write("usb", "no console on USB");
        foreach (var c in consoles)
            Log.Write("usb", $"{(c.Native ? "native " : "")}console found on USB, " + (c.Openable ? "can be opened" : $"cannot be opened: {c.ErrorName}"));
        Changed?.Invoke(consoles);
    }
}
