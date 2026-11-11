using System.Diagnostics;
using NXStream.Core;
using NXStream.Net;
using NXStream.Render;

namespace NXStream.UI;

public sealed class OptionsForm : Form
{
    public event Action? Changed;

    private const int UsbExtra = 56;
    private const int ConnectionShift = 60;

    private readonly List<Control> _below = [];
    private readonly MainFlow? _flow;
    private GroupBox? _connection;
    private Label? _usbStatus;
    private Button[] _usbButtons = [];
    private bool _usbShown;
    private bool _usbBusy;

    public OptionsForm(AppSettings settings, MainFlow? flow = null)
    {
        _flow = flow;
        _usbShown = flow is not null && settings.Connection == ConnectionMode.Usb;
        var top = flow is null ? 0 : ConnectionShift + (_usbShown ? UsbExtra : 0);
        AutoScaleDimensions = new SizeF(96F, 96F);
        AutoScaleMode = AutoScaleMode.Dpi;
        Text = MainText.OptionsTitle;
        Font = SystemFonts.MessageBoxFont ?? Font;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = MinimizeBox = false;
        ShowInTaskbar = false;
        StartPosition = FormStartPosition.CenterParent;
        ClientSize = new Size(420, 476 + top);

        var image = new GroupBox { Text = MainText.ImageGroup, Location = new Point(12, 10 + top), Size = new Size(396, 196) };
        var scalers = MainText.Scalers(true);
        var shown = MainText.ShownScaler(settings, true);
        for (var i = 0; i < scalers.Length; i++)
        {
            var (scaler, text) = scalers[i];
            var radio = new RadioButton
            {
                Text = text, AutoSize = true, Location = new Point(14, 22 + i * 23), Checked = shown == scaler,
            };
            radio.CheckedChanged += (_, _) =>
            {
                if (!radio.Checked) return;
                settings.Scaler = scaler;
                Changed?.Invoke();
            };
            image.Controls.Add(radio);
        }
        image.Controls.Add(new Label { Text = MainText.SharpnessLabel, AutoSize = true, Location = new Point(14, 120) });
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
        var sharpnessValue = new Label { Text = MainText.SharpnessText(sharpness.Value), AutoSize = true, Location = new Point(342, 120) };
        sharpness.ValueChanged += (_, _) =>
        {
            settings.Sharpness = sharpness.Value;
            sharpnessValue.Text = MainText.SharpnessText(sharpness.Value);
            Changed?.Invoke();
        };
        image.Controls.Add(sharpness);
        image.Controls.Add(sharpnessValue);
        image.Controls.Add(new Label
        {
            Text = MainText.SharpnessHint,
            Location = new Point(14, 160),
            Size = new Size(370, 30),
            ForeColor = SystemColors.GrayText,
        });
        Controls.Add(image);
        _below.Add(image);

        var audio = new GroupBox { Text = MainText.AudioGroup, Location = new Point(12, 214 + top), Size = new Size(396, 78) };
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
        var percent = new Label { Text = MainText.PercentText(volume.Value), AutoSize = true, Location = new Point(270, 28) };
        var mute = new CheckBox { Text = MainText.MuteLabel, AutoSize = true, Location = new Point(320, 27), Checked = settings.Muted };
        volume.ValueChanged += (_, _) =>
        {
            settings.VolumePercent = volume.Value;
            percent.Text = MainText.PercentText(volume.Value);
            Changed?.Invoke();
        };
        mute.CheckedChanged += (_, _) =>
        {
            settings.Muted = mute.Checked;
            volume.Enabled = !mute.Checked;
            Changed?.Invoke();
        };
        audio.Controls.Add(volume);
        audio.Controls.Add(percent);
        audio.Controls.Add(mute);
        Controls.Add(audio);
        _below.Add(audio);

        var general = new GroupBox { Text = MainText.GeneralGroup, Location = new Point(12, 300 + top), Size = new Size(396, 124) };
        var y = 22;
        foreach (var (text, value, apply) in MainText.GeneralChecks(settings))
        {
            var box = new CheckBox { Text = text, AutoSize = true, Location = new Point(14, y), Checked = value };
            box.CheckedChanged += (_, _) =>
            {
                apply(box.Checked);
                Changed?.Invoke();
            };
            general.Controls.Add(box);
            y += 24;
        }
        Controls.Add(general);
        _below.Add(general);

        var logs = new LinkLabel { Text = MainText.LogFolderLabel, AutoSize = true, Location = new Point(14, 442 + top) };
        logs.LinkClicked += (_, _) =>
        {
            try { Process.Start(new ProcessStartInfo(Log.Folder) { UseShellExecute = true }); }
            catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException) { }
        };
        Controls.Add(logs);
        _below.Add(logs);
        var ok = new Button { Text = MainText.OkButton, DialogResult = DialogResult.OK, Location = new Point(332, 436 + top), Size = new Size(76, 27) };
        Controls.Add(ok);
        _below.Add(ok);
        if (flow is not null) BuildConnection(settings, flow);
        AcceptButton = CancelButton = ok;
        WindowsTheme.Apply(this);
    }

    private void BuildConnection(AppSettings settings, MainFlow flow)
    {
        var group = new GroupBox
        {
            Text = UsbText.ConnectionLabel,
            Location = new Point(12, 10),
            Size = new Size(396, _usbShown ? 52 + UsbExtra : 52),
        };
        var network = new RadioButton
        {
            Text = UsbText.Network, AutoSize = true, Location = new Point(14, 22), Checked = settings.Connection == ConnectionMode.Network,
        };
        var usb = new RadioButton
        {
            Text = UsbText.Usb, AutoSize = true, Location = new Point(120, 22), Checked = settings.Connection == ConnectionMode.Usb,
        };
        _usbStatus = new Label
        {
            AutoSize = false, AutoEllipsis = true, Location = new Point(14, 52), Size = new Size(368, 20),
            Text = UsbText.StatusText(UsbDriverStatus.Unknown), Visible = _usbShown,
        };
        var labels = new[] { UsbText.InstallButton, UsbText.RepairButton, UsbText.UninstallButton };
        _usbButtons = new Button[3];
        for (var i = 0; i < 3; i++)
        {
            _usbButtons[i] = new Button
            {
                Text = labels[i], Size = new Size(84, 27), Location = new Point(118 + i * 90, 76), Enabled = false, Visible = _usbShown,
            };
            group.Controls.Add(_usbButtons[i]);
        }
        _usbButtons[0].Click += async (_, _) => await RunDriver(flow.InstallUsbDriverAsync);
        _usbButtons[1].Click += async (_, _) => await RunDriver(flow.RepairUsbDriverAsync);
        _usbButtons[2].Click += async (_, _) =>
        {
            if (MessageBox.Show(this, UsbText.UninstallConfirmWindows, "NXStream", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return;
            await RunDriver(flow.UninstallUsbDriverAsync);
        };
        usb.CheckedChanged += (_, _) =>
        {
            if (!usb.Checked) return;
            flow.SetConnectionMode(ConnectionMode.Usb);
            ShowUsbRows(true);
        };
        network.CheckedChanged += (_, _) =>
        {
            if (!network.Checked) return;
            flow.SetConnectionMode(ConnectionMode.Network);
            ShowUsbRows(false);
        };
        group.Controls.Add(network);
        group.Controls.Add(usb);
        group.Controls.Add(_usbStatus);
        _connection = group;
        Controls.Add(group);
        if (_usbShown) RefreshUsbStatus();
    }

    private void ShowUsbRows(bool show)
    {
        if (_connection is null || show == _usbShown) return;
        _usbShown = show;
        var delta = LogicalToDeviceUnits(show ? UsbExtra : -UsbExtra);
        _connection.Height += delta;
        foreach (var c in _below) c.Top += delta;
        Height += delta;
        _usbStatus!.Visible = show;
        foreach (var b in _usbButtons) b.Visible = show;
        if (show) RefreshUsbStatus();
    }

    private void RefreshUsbStatus()
    {
        if (_flow is null) return;
        var flow = _flow;
        _ = Task.Run(() =>
        {
            var status = flow.UsbDriverStatus;
            try { BeginInvoke(() => ApplyUsbStatus(status)); }
            catch (Exception ex) when (ex is InvalidOperationException or ObjectDisposedException) { }
        });
    }

    private void ApplyUsbStatus(UsbDriverStatus status)
    {
        if (_usbStatus is null || IsDisposed) return;
        _usbStatus.Text = UsbText.StatusText(status);
        var usable = !_usbBusy;
        _usbButtons[0].Enabled = usable && status is UsbDriverStatus.NotInstalled or UsbDriverStatus.Unknown;
        _usbButtons[1].Enabled = usable && status is UsbDriverStatus.Ready or UsbDriverStatus.NoPermission;
        _usbButtons[2].Enabled = usable && status == UsbDriverStatus.Ready;
    }

    private async Task RunDriver(Func<Task<bool>> action)
    {
        _usbBusy = true;
        foreach (var b in _usbButtons) b.Enabled = false;
        _usbStatus!.Text = UsbText.DriverWorking;
        var ok = await action();
        _usbBusy = false;
        if (IsDisposed) return;
        if (!ok) _usbStatus.Text = UsbText.DriverFailed;
        var status = await Task.Run(() => _flow!.UsbDriverStatus);
        if (IsDisposed) return;
        ApplyUsbStatus(status);
        if (!ok) _usbStatus.Text = UsbText.DriverFailed;
    }
}
