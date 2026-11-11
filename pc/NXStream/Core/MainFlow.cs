using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;
using NXStream.Media;
using NXStream.Net;

namespace NXStream.Core;

public interface IMainHost
{
    string Address { get; set; }
    void Post(Action action);
    void SetStatus(string text);
    void SetEnabled(bool enabled);
    void ReplaceDevices(IReadOnlyList<SwitchDevice> devices, int selectIndex);
    void OpenViewer(ViewerSession session, IPEndPoint endpoint, string ffmpegPath, Action<string?> closed);
    void AskUpdate(UpdateOffer offer, Action accepted);
    void Quit();
    void SetUsbDriverStatus(UsbDriverStatus status) { }
}

public sealed class MainFlow
{
    private readonly IMainHost _host;
    private readonly IMediaFactory _media;
    private readonly string? _autoConnectTarget;
    private readonly CancellationTokenSource _lifetime = new();
    private readonly string? _ffmpegPath = FfmpegLocator.Find();
    private volatile bool _busy;
    private bool _rebuilding;
    private IReadOnlyList<SwitchDevice> _devices = [];
    private SwitchDevice? _selected;
    private IReadOnlyList<SwitchDevice> _networkDevices = [];
    private IReadOnlyList<SwitchDevice> _usbDevices = [];
    private IReadOnlyList<UsbConsole> _usbConsoles = [];
    private UsbDiscovery? _usb;
    private CancellationTokenSource? _beaconCts;
    private bool _askedDriver;
    private bool _driverDeclined;

    private bool UsbMode => Settings.Connection == ConnectionMode.Usb;

    public AppSettings Settings { get; } = AppSettings.Load();

    public MainFlow(IMainHost host, IMediaFactory media, string? autoConnectTarget)
    {
        _host = host;
        _media = media;
        _autoConnectTarget = autoConnectTarget;
    }

    public void Start()
    {
        _host.SetStatus(_ffmpegPath is null ? MainText.FfmpegMissing : MainText.Searching);

        LocalNetwork.LogAdapters();
        if (!UsbMode) StartBeacons();
        _ = new DeviceWatcher(() => Settings.LastTarget, () => !_busy && !UsbMode, devices => Ui(() =>
            {
                _networkDevices = devices;
                if (!UsbMode) ShowDevices(devices);
            }))
            .RunAsync(_lifetime.Token);
        if (UsbMode) StartUsb();

        if (_autoConnectTarget is not null && EndpointParser.TryParse(_autoConnectTarget, out var ep))
            Ui(() => Connect(ep, ep.Address.ToString(), "command line"));

        _ = CheckForUpdateAsync();
    }

    private async Task CheckForUpdateAsync()
    {
        try
        {
            if (!SelfReplace.TryGetTarget(out _))
            {
                Log.Write("update", "update check skipped: no replaceable target");
                return;
            }
            var offer = await new UpdateClient().CheckAsync(_lifetime.Token).ConfigureAwait(false);
            if (offer is null) return;
            Ui(() =>
            {
                if (_busy)
                {
                    Log.Write("update", "update check skipped: busy");
                    return;
                }
                Log.Write("update", $"asking to update to {offer.Version}");
                _host.AskUpdate(offer, () => ApplyUpdate(offer));
            });
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            Log.Write("update", $"check aborted: {ex.GetType().Name}");
        }
    }

    private void ApplyUpdate(UpdateOffer offer)
    {
        if (_busy) return;
        if (!SelfReplace.TryGetTarget(out var target)) return;
        _busy = true;
        _host.SetEnabled(false);
        Log.Write("update", "update accepted");
        var dir = Path.GetDirectoryName(target) ?? ".";
        if (!SelfReplace.CanWrite(dir))
        {
            FailUpdate(UpdateText.ReasonWrite);
            return;
        }
        _host.SetStatus(UpdateText.Downloading(0));
        var launchArgs = _autoConnectTarget is null ? Array.Empty<string>() : new[] { _autoConnectTarget };
        _ = Task.Run(async () =>
        {
            var tmp = SelfReplace.TempPath(target);
            try
            {
                var progress = new Progress<int>(pct => Ui(() => _host.SetStatus(UpdateText.Downloading(pct))));
                await new UpdateClient().DownloadAsync(offer, tmp, progress, _lifetime.Token).ConfigureAwait(false);
                Ui(() => _host.SetStatus(UpdateText.Verifying));
                Log.Write("update", "download verified");
                SelfReplace.Swap(target, tmp);
                Ui(() => _host.SetStatus(UpdateText.Restarting));
                SelfReplace.Relaunch(target, launchArgs);
                Ui(_host.Quit);
            }
            catch (OperationCanceledException)
            {
                Log.Write("update", "update cancelled");
            }
            catch (UpdateException ex)
            {
                Ui(() => FailUpdate(ex.Message));
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
            {
                Log.Write("update", $"update step failed: {ex.GetType().Name}");
                Ui(() => FailUpdate(UpdateText.ReasonWrite));
            }
        });
    }

    private void FailUpdate(string reason)
    {
        Log.Write("update", $"update failed: {reason}");
        _host.SetStatus(UpdateText.Failed(reason));
        Release();
    }

    public void Shutdown()
    {
        Settings.Save();
        _lifetime.Cancel();
    }

    public UsbDriverStatus UsbDriverStatus => UsbPlatform.Access?.GetStatus() ?? UsbDriverStatus.Unknown;

    public void SetConnectionMode(ConnectionMode mode)
    {
        if (Settings.Connection == mode) return;
        Settings.Connection = mode;
        Settings.Save();
        Log.Write("app", $"connection mode {mode}");
        _selected = null;
        if (mode == ConnectionMode.Usb)
        {
            StopBeacons();
            StartUsb();
            ShowDevices(_usbDevices);
            _usb?.Rescan();
        }
        else
        {
            StartBeacons();
            ShowDevices(_networkDevices);
        }
    }

    public Task<bool> InstallUsbDriverAsync() => RunDriverAction(a => a.InstallAsync());

    public Task<bool> RepairUsbDriverAsync() => RunDriverAction(a => a.RepairAsync());

    public Task<bool> UninstallUsbDriverAsync() => RunDriverAction(a => a.UninstallAsync());

    private void StartBeacons()
    {
        if (_beaconCts is not null) return;
        _beaconCts = CancellationTokenSource.CreateLinkedTokenSource(_lifetime.Token);
        DiscoveryClient.StartBeaconListener(_beaconCts.Token);
    }

    private void StopBeacons()
    {
        if (_beaconCts is null) return;
        _beaconCts.Cancel();
        _beaconCts = null;
        DiscoveryClient.ForgetBeaconListener();
    }

    private void StartUsb()
    {
        if (_usb is not null) return;
        _usb = new UsbDiscovery(() => !_busy && UsbMode);
        _usb.Changed += consoles => Ui(() => OnUsbChanged(consoles));
        _ = _usb.RunAsync(_lifetime.Token);
    }

    private void OnUsbChanged(IReadOnlyList<UsbConsole> consoles)
    {
        _usbConsoles = consoles;
        _usbDevices = consoles.Select(c => new SwitchDevice(c.Native ? (NativeDiscovery.KnownName ?? UsbText.UsbNativeConsole) : UsbText.ConsoleName(c.Name), UsbTarget.Address, c.Native ? 1 : 0, false)).ToList();
        if (!UsbMode) return;
        ShowDevices(_usbDevices);
        OfferDriver();
    }

    private void OfferDriver()
    {
        if (_askedDriver || _busy) return;
        if (!_usbConsoles.Any(c => c.NeedsDriver)) return;
        var access = UsbPlatform.Access;
        if (access is null) return;
        var status = access.GetStatus();
        Log.Write("usb", $"console cannot be opened, driver status {status}");
        if (status is not (UsbDriverStatus.NotInstalled or UsbDriverStatus.NoPermission)) return;
        _askedDriver = true;
        var accepted = UsbPlatform.AskInstall?.Invoke(UsbText.DriverPrompt) ?? false;
        Log.Write("usb", accepted ? "driver install accepted" : "driver install declined");
        if (!accepted)
        {
            _driverDeclined = true;
            _host.SetStatus(UsbText.DriverDeclined);
            return;
        }
        _ = RunDriverAction(a => a.InstallAsync());
    }

    private async Task<bool> RunDriverAction(Func<IUsbAccess, Task<bool>> action)
    {
        var access = UsbPlatform.Access;
        if (access is null) return false;
        Ui(() => _host.SetStatus(UsbText.DriverWorking));
        var ok = false;
        var status = UsbDriverStatus.Unknown;
        try
        {
            ok = await action(access).ConfigureAwait(false);
            status = access.GetStatus();
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            Log.Error("usb", "driver action failed", ex);
        }
        Log.Write("usb", $"driver action {(ok ? "succeeded" : "failed")}, status {status}");
        Ui(() =>
        {
            _host.SetUsbDriverStatus(status);
            if (ok) _driverDeclined = false;
            _host.SetStatus(ok ? MainText.Searching : UsbText.DriverFailed);
            _usb?.Rescan();
        });
        return ok;
    }

    public void OnSelectionChanged(int index)
    {
        if (_rebuilding) return;
        _selected = index >= 0 && index < _devices.Count ? _devices[index] : null;
        if (_selected is not null && !UsbMode) _host.Address = _selected.Address.ToString();
    }

    public void Activate(int index)
    {
        if (index < 0 || index >= _devices.Count) return;
        var d = _devices[index];
        Connect(new IPEndPoint(d.Address, d.Port), d.Name, "discovered console");
    }

    public void ConnectTyped()
    {
        if (UsbMode)
        {
            if (_devices.Count > 0)
            {
                var device = _selected ?? _devices[0];
                Connect(new IPEndPoint(device.Address, device.Port), device.Name, "usb console");
            }
            else _host.SetStatus(UsbText.NoUsbConsole);
            return;
        }
        if (EndpointParser.TryParse(_host.Address, out var ep))
        {
            var name = _devices.FirstOrDefault(d => d.Address.Equals(ep.Address))?.Name;
            Connect(ep, name ?? ep.Address.ToString(), "typed address");
        }
        else
        {
            _host.SetStatus(MainText.InvalidAddress);
        }
    }

    private void Ui(Action action) => _host.Post(() =>
    {
        if (!_lifetime.IsCancellationRequested) action();
    });

    private void ShowDevices(IReadOnlyList<SwitchDevice> devices)
    {
        var selectIndex = -1;
        for (var i = 0; i < devices.Count; i++)
            if (_selected is not null && devices[i].Address.Equals(_selected.Address) && devices[i].Port == _selected.Port) selectIndex = i;
        if (selectIndex < 0 && devices.Count > 0) selectIndex = 0;

        _rebuilding = true;
        try { _host.ReplaceDevices(devices, selectIndex); }
        finally { _rebuilding = false; }
        _devices = devices;
        _selected = selectIndex >= 0 ? devices[selectIndex] : null;
        if (!UsbMode && _host.Address.Length == 0 && _selected is not null) _host.Address = _selected.Address.ToString();

        if (_busy || _ffmpegPath is null) return;
        _host.SetStatus(IdleStatus(devices));
    }

    private string IdleStatus(IReadOnlyList<SwitchDevice> devices)
    {
        if (!UsbMode) return devices.Count > 0 ? MainText.DoubleClickHint : MainText.NoConsole;
        if (_usb?.LibraryMissing == true) return UsbText.LibraryMissing;
        if (devices.Count == 0) return UsbText.NoUsbConsole;
        return _driverDeclined && _usbConsoles.Any(c => c.NeedsDriver) ? UsbText.DriverDeclined : MainText.DoubleClickHint;
    }

    private async void Connect(IPEndPoint endpoint, string label, string source)
    {
        if (_busy) return;
        if (_ffmpegPath is null)
        {
            _host.SetStatus(MainText.FfmpegMissing);
            return;
        }

        if (UsbTarget.Is(endpoint) && _driverDeclined && _usbConsoles.Any(c => c.NeedsDriver))
        {
            _host.SetStatus(UsbText.DriverDeclined);
            return;
        }

        _busy = true;
        _host.SetEnabled(false);
        _host.SetStatus(MainText.Connecting(label));
        Log.Write("app", $"connect: {source}");

        ViewerSession? session = null;
        string? error = null;
        try
        {
            session = await ViewerSession.ConnectAsync(endpoint, _ffmpegPath, Settings.EffectiveVolume,
                                                       Settings.Render, _media, _lifetime.Token);
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            Log.Write("app", $"connection failed: {(ex is SocketException se ? se.SocketErrorCode.ToString() : ex is UsbException ue0 ? ue0.ErrorName : ex.GetType().Name)}");
            error = ex switch
            {
                UsbException { Code: LibUsb.NoDevice } => UsbText.NoUsbConsole,
                UsbException { NeedsDriver: true } when _driverDeclined => UsbText.DriverDeclined,
                UsbException ue => UsbText.OpenFailed(ue.ErrorName),
                DllNotFoundException => UsbText.LibraryMissing,
                _ => ex.Message,
            };
        }
        _host.Post(() => Connected(endpoint, session, error));
    }

    private void Connected(IPEndPoint endpoint, ViewerSession? session, string? error)
    {
        if (_lifetime.IsCancellationRequested)
        {
            session?.Stop();
            return;
        }
        if (session is null)
        {
            if (error is not null) _host.SetStatus(error);
            Release();
            return;
        }

        if (!UsbTarget.Is(endpoint))
        {
            Settings.LastTarget = endpoint.Address.ToString();
            Settings.Save();
        }
        var done = false;
        _host.OpenViewer(session, endpoint, _ffmpegPath!, reason =>
        {
            if (done) return;
            done = true;
            if (_lifetime.IsCancellationRequested) return;
            _host.SetStatus(reason ?? "");
            Release();
        });
    }

    private void Release()
    {
        _busy = false;
        _host.SetEnabled(true);
        _usb?.Rescan();
    }
}
