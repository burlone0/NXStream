using System.Net;
using NXStream.Core;
using NXStream.Media;
using NXStream.Net;

namespace NXStream.UI;

public sealed class MainForm : Form, IMainHost
{
    private readonly MainFlow _flow;

    private readonly ListView _list = new()
    {
        View = View.Details,
        FullRowSelect = true,
        MultiSelect = false,
        HeaderStyle = ColumnHeaderStyle.Nonclickable,
        HideSelection = false,
    };
    private readonly TextBox _addressBox = new() { PlaceholderText = MainText.AddressPlaceholder };
    private readonly Button _connect = new() { Text = MainText.ConnectButton };
    private readonly Button _options = new() { Text = MainText.OptionsButton };
    private readonly Label _status = new() { AutoEllipsis = true };
    private readonly Label _foundLabel = new() { Text = MainText.FoundLabel, AutoSize = true, Location = new Point(14, 14) };
    private readonly Label _addressLabel = new() { Text = MainText.AddressLabel, AutoSize = true, Location = new Point(14, 222) };

    public MainForm(string? autoConnectTarget = null)
    {
        _flow = new MainFlow(this, new WindowsMedia(), autoConnectTarget);
        AutoScaleDimensions = new SizeF(96F, 96F);
        AutoScaleMode = AutoScaleMode.Dpi;
        Text = "NXStream";
        Font = SystemFonts.MessageBoxFont ?? Font;
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(440, 330);
        try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); }
        catch (ArgumentException) { }

        Controls.Add(_foundLabel);
        _list.Location = new Point(14, 36);
        _list.Size = new Size(412, 170);
        _list.Columns.Add(MainText.NameColumn, 170);
        _list.Columns.Add(MainText.AddressColumn, 120);
        _list.Columns.Add(MainText.StateColumn, 100);
        _list.HandleCreated += (_, _) => FillLastColumn();
        Controls.Add(_list);

        Controls.Add(_addressLabel);
        _addressBox.Location = new Point(14, 244);
        _addressBox.Size = new Size(316, 23);
        _connect.Location = new Point(338, 242);
        _connect.Size = new Size(88, 27);
        Controls.Add(_addressBox);
        Controls.Add(_connect);
        AcceptButton = _connect;

        _status.Location = new Point(14, 280);
        _status.Size = new Size(316, 40);
        _options.Location = new Point(338, 290);
        _options.Size = new Size(88, 27);
        Controls.Add(_status);
        Controls.Add(_options);

        _list.SelectedIndexChanged += (_, _) =>
            _flow.OnSelectionChanged(_list.SelectedIndices.Count > 0 ? _list.SelectedIndices[0] : -1);
        _list.ItemActivate += (_, _) =>
        {
            if (_list.SelectedIndices.Count > 0) _flow.Activate(_list.SelectedIndices[0]);
        };
        _connect.Click += (_, _) => _flow.ConnectTyped();
        _options.Click += (_, _) =>
        {
            using var options = new OptionsForm(_flow.Settings, _flow);
            options.ShowDialog(this);
            _flow.Settings.Save();
            ApplyConnectionMode();
        };
        ApplyConnectionMode();
        WindowsTheme.Apply(this);
    }

    private void ApplyConnectionMode()
    {
        var network = _flow.Settings.Connection == ConnectionMode.Network;
        _addressLabel.Visible = _addressBox.Visible = network;
        _foundLabel.Text = network ? MainText.FoundLabel : MainText.FoundLabelUsb;
    }

    public string Address
    {
        get => _addressBox.Text;
        set => _addressBox.Text = value;
    }

    public void Post(Action action)
    {
        try { BeginInvoke(action); }
        catch (InvalidOperationException) { }
    }

    public void SetStatus(string text) => _status.Text = text;

    public void SetEnabled(bool enabled) => _connect.Enabled = _list.Enabled = enabled;

    public void ReplaceDevices(IReadOnlyList<SwitchDevice> devices, int selectIndex)
    {
        _list.BeginUpdate();
        _list.Items.Clear();
        foreach (var d in devices) _list.Items.Add(new ListViewItem(MainText.DeviceCells(d)));
        if (selectIndex >= 0) _list.Items[selectIndex].Selected = true;
        _list.EndUpdate();
        FillLastColumn();
    }

    public void OpenViewer(ViewerSession session, IPEndPoint endpoint, string ffmpegPath, Action<string?> closed)
    {
        string? endReason;
        try
        {
            using var viewer = new ViewerForm(session, _flow.Settings, endpoint, ffmpegPath);
            Hide();
            viewer.ShowDialog();
            endReason = viewer.EndReason;
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            Log.Error("app", "viewer failed", ex);
            session.Stop();
            endReason = ex.Message;
        }
        Show();
        Activate();
        closed(endReason);
    }

    public void AskUpdate(UpdateOffer offer, Action accepted)
    {
        using var dialog = new UpdateForm(offer);
        if (dialog.ShowDialog(this) == DialogResult.OK) accepted();
    }

    public void Quit() => Close();

    protected override void OnLoad(EventArgs e)
    {
        base.OnLoad(e);
        _flow.Start();
    }

    private void FillLastColumn()
    {
        var used = _list.Columns[0].Width + _list.Columns[1].Width;
        _list.Columns[2].Width = Math.Max(60, _list.ClientSize.Width - used);
    }

    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        _flow.Shutdown();
        base.OnFormClosing(e);
    }
}
