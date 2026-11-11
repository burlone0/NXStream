using System.Diagnostics;
using System.Drawing;
using NXStream.Core;
using NXStream.Linux.Ui;
using NXStream.Net;
using NXStream.Render;

namespace NXStream.Linux;

internal sealed class OptionsWindow
{
    private const int BaseHeight = 476;
    private const int ConnectionShift = 60;
    private const int UsbExtra = 56;

    private readonly UiWindow _window;
    private readonly MainFlow? _flow;
    private readonly Container _body = new();
    private readonly GroupBox _connection = new();
    private readonly Label _usbStatus = new();
    private readonly Button _install = new() { Text = UsbText.InstallButton };
    private readonly Button _repair = new() { Text = UsbText.RepairButton };
    private readonly Button _uninstall = new() { Text = UsbText.UninstallButton };
    private UsbDriverStatus _status = UsbDriverStatus.Unknown;
    private bool _closed, _working;

    public event Action? Changed;
    public event Action? Closed;

    public OptionsWindow(AppSettings settings, IntPtr parent, MainFlow? flow = null)
    {
        _flow = flow;
        var top = flow is null ? 0 : ConnectionShift + (settings.Connection == ConnectionMode.Usb ? UsbExtra : 0);
        _window = new UiWindow(MainText.OptionsTitle, 420, BaseHeight + top, App.Palette);
        _window.Closed += Close;
        _body.Bounds = new Rectangle(0, top, 420, BaseHeight);
        _window.Add(_body);
        if (flow is not null) BuildConnection(settings, flow);

        var image = new GroupBox { Text = MainText.ImageGroup, Location = new Point(12, 10), Size = new Size(396, 196) };
        var scalers = MainText.Scalers(true);
        var shown = MainText.ShownScaler(settings, false);
        _body.Add(image);
        for (var i = 0; i < scalers.Length; i++)
        {
            var (scaler, text) = scalers[i];
            var radio = new RadioButton
            {
                Text = text,
                Location = new Point(14, 22 + i * 23),
                Size = new Size(1, 20),
                Checked = shown == scaler,
                Enabled = scaler != Scaler.SuperResolution,
            };
            radio.CheckedChanged += () =>
            {
                if (!radio.Checked) return;
                settings.Scaler = scaler;
                Changed?.Invoke();
            };
            image.Add(radio);
        }
        image.Add(new Label { Text = MainText.SharpnessLabel, Location = new Point(14, 120) });
        var sharpness = new TrackBar
        {
            Minimum = 0,
            Maximum = 100,
            TickFrequency = 10,
            SmallChange = 5,
            LargeChange = 10,
            Value = Math.Clamp(settings.Sharpness, 0, 100),
            Location = new Point(140, 114),
            Size = new Size(200, 45),
        };
        var sharpnessValue = new Label { Text = MainText.SharpnessText(sharpness.Value), Location = new Point(342, 120) };
        sharpness.ValueChanged += () =>
        {
            settings.Sharpness = sharpness.Value;
            sharpnessValue.Text = MainText.SharpnessText(sharpness.Value);
            Changed?.Invoke();
        };
        image.Add(sharpness);
        image.Add(sharpnessValue);
        image.Add(new Label
        {
            Text = MainText.SharpnessHint,
            Location = new Point(14, 160),
            Size = new Size(370, 30),
            Wrap = true,
            Muted = true,
        });

        var audio = new GroupBox { Text = MainText.AudioGroup, Location = new Point(12, 214), Size = new Size(396, 78) };
        _body.Add(audio);
        var volume = new TrackBar
        {
            Minimum = 0,
            Maximum = 100,
            TickFrequency = 10,
            SmallChange = 5,
            LargeChange = 10,
            Value = Math.Clamp(settings.VolumePercent, 0, 100),
            Location = new Point(8, 22),
            Size = new Size(260, 45),
            Enabled = !settings.Muted,
        };
        var percent = new Label { Text = MainText.PercentText(volume.Value), Location = new Point(270, 28) };
        var mute = new CheckBox { Text = MainText.MuteLabel, Location = new Point(320, 27), Size = new Size(1, 20), Checked = settings.Muted };
        volume.ValueChanged += () =>
        {
            settings.VolumePercent = volume.Value;
            percent.Text = MainText.PercentText(volume.Value);
            Changed?.Invoke();
        };
        mute.CheckedChanged += () =>
        {
            settings.Muted = mute.Checked;
            volume.Enabled = !mute.Checked;
            Changed?.Invoke();
        };
        audio.Add(volume);
        audio.Add(percent);
        audio.Add(mute);

        var general = new GroupBox { Text = MainText.GeneralGroup, Location = new Point(12, 300), Size = new Size(396, 124) };
        _body.Add(general);
        var y = 22;
        foreach (var (text, value, apply) in MainText.GeneralChecks(settings))
        {
            var box = new CheckBox { Text = text, Location = new Point(14, y), Size = new Size(1, 20), Checked = value };
            box.CheckedChanged += () =>
            {
                apply(box.Checked);
                Changed?.Invoke();
            };
            general.Add(box);
            y += 24;
        }

        var logs = new LinkLabel { Text = MainText.LogFolderLabel, Location = new Point(14, 442), Size = new Size(1, 15) };
        logs.Click += OpenLogFolder;
        _body.Add(logs);
        var ok = new Button { Text = MainText.OkButton, Location = new Point(332, 436), Size = new Size(76, 27) };
        ok.Click += Close;
        _body.Add(ok);
        _window.AcceptButton = _window.CancelButton = ok;

        _window.SetModalFor(parent);
        App.Modal = _window;
    }

    private void BuildConnection(AppSettings settings, MainFlow flow)
    {
        var usb = settings.Connection == ConnectionMode.Usb;
        _connection.Text = UsbText.ConnectionLabel;
        _connection.Bounds = new Rectangle(12, 10, 396, usb ? 52 + UsbExtra : 52);
        _window.Add(_connection);
        var network = new RadioButton { Text = UsbText.Network, Location = new Point(14, 22), Size = new Size(1, 20), Checked = !usb };
        var usbRadio = new RadioButton { Text = UsbText.Usb, Location = new Point(120, 22), Size = new Size(1, 20), Checked = usb };
        _connection.Add(network);
        _connection.Add(usbRadio);
        _usbStatus.Location = new Point(14, 52);
        _usbStatus.Size = new Size(368, 20);
        _connection.Add(_usbStatus);
        _install.Bounds = new Rectangle(118, 76, 84, 27);
        _repair.Bounds = new Rectangle(208, 76, 84, 27);
        _uninstall.Bounds = new Rectangle(298, 76, 84, 27);
        _connection.Add(_install);
        _connection.Add(_repair);
        _connection.Add(_uninstall);
        _install.Click += () => RunDriver(flow.InstallUsbDriverAsync, false);
        _repair.Click += () => RunDriver(flow.RepairUsbDriverAsync, false);
        _uninstall.Click += () =>
        {
            if (!NativeDialog.Ask(UsbText.UninstallConfirmLinux, UsbText.UninstallButton, UpdateText.Cancel)) return;
            RunDriver(flow.UninstallUsbDriverAsync, true);
        };
        usbRadio.CheckedChanged += () =>
        {
            if (usbRadio.Checked) SetMode(ConnectionMode.Usb);
        };
        network.CheckedChanged += () =>
        {
            if (network.Checked) SetMode(ConnectionMode.Network);
        };
        ApplyMode(usb);
        if (usb) RefreshStatus(flow.UsbDriverStatus);
    }

    private void SetMode(ConnectionMode mode)
    {
        _flow!.SetConnectionMode(mode);
        ApplyMode(mode == ConnectionMode.Usb);
        if (mode == ConnectionMode.Usb) RefreshStatus(_flow.UsbDriverStatus);
        Changed?.Invoke();
    }

    private void ApplyMode(bool usb)
    {
        var top = ConnectionShift + (usb ? UsbExtra : 0);
        _usbStatus.Visible = _install.Visible = _repair.Visible = _uninstall.Visible = usb;
        _connection.Size = new Size(396, usb ? 52 + UsbExtra : 52);
        _body.Location = new Point(0, top);
        _window.Resize(420, BaseHeight + top);
    }

    private void RefreshStatus(UsbDriverStatus status)
    {
        _status = status;
        _usbStatus.Text = UsbText.StatusText(status);
        UpdateButtons();
    }

    private void UpdateButtons()
    {
        _install.Enabled = !_working && _status is UsbDriverStatus.NotInstalled or UsbDriverStatus.Unknown;
        _repair.Enabled = !_working && _status is UsbDriverStatus.Ready or UsbDriverStatus.NoPermission;
        _uninstall.Enabled = !_working && _status == UsbDriverStatus.Ready;
    }

    private async void RunDriver(Func<Task<bool>> action, bool remove)
    {
        if (_working) return;
        _working = true;
        _usbStatus.Text = UsbText.DriverWorking;
        UpdateButtons();
        var ok = false;
        try
        {
            ok = await action().ConfigureAwait(false);
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            Log.Error("usb", "driver action failed", ex);
        }
        App.Post(() =>
        {
            _working = false;
            if (_closed) return;
            RefreshStatus(_flow!.UsbDriverStatus);
            if (ok) return;
            _usbStatus.Text = UsbText.DriverFailed;
            NativeDialog.Inform(UsbText.DriverFailed + "\n\n" + UdevUsbAccess.ManualCommand(remove));
        });
    }

    public void Show() => _window.Show();

    public void Close()
    {
        if (_closed) return;
        _closed = true;
        if (App.Modal == _window) App.Modal = null;
        _window.Hide();
        Closed?.Invoke();
        App.Post(_window.Dispose);
    }

    private static void OpenLogFolder()
    {
        try
        {
            var start = new ProcessStartInfo("xdg-open") { UseShellExecute = false };
            start.ArgumentList.Add(Log.Folder);
            start.Environment.Remove("LD_LIBRARY_PATH");
            Process.Start(start)?.Dispose();
        }
        catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            Log.Write("ui", $"xdg-open failed: {ex.GetType().Name}");
        }
    }
}
