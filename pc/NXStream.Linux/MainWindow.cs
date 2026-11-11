using System.Drawing;
using System.Net;
using NXStream.Core;
using NXStream.Linux.Ui;
using NXStream.Media;
using NXStream.Net;

namespace NXStream.Linux;

internal sealed class MainWindow : IMainHost
{
    private readonly MainFlow _flow;
    private readonly IMediaFactory _media;
    private readonly UiWindow _window = new("NXStream", 440, 330, App.Palette);

    private readonly ListView _list = new() { Bounds = new Rectangle(14, 36, 412, 170) };
    private readonly TextBox _addressBox = new() { PlaceholderText = MainText.AddressPlaceholder, Bounds = new Rectangle(14, 244, 316, 23) };
    private readonly Button _connect = new() { Text = MainText.ConnectButton, Bounds = new Rectangle(338, 242, 88, 27) };
    private readonly Button _options = new() { Text = MainText.OptionsButton, Bounds = new Rectangle(338, 290, 88, 27) };
    private readonly Label _foundLabel = new() { Text = MainText.FoundLabel, Location = new Point(14, 14) };
    private readonly Label _addressLabel = new() { Text = MainText.AddressLabel, Location = new Point(14, 222) };
    private readonly Label _status = new() { Bounds = new Rectangle(14, 280, 316, 40), Wrap = true };

    public event Action? Closed;

    public UiWindow Window => _window;


    public MainWindow(IMediaFactory media, string? autoConnectTarget)
    {
        _media = media;
        _flow = new MainFlow(this, media, autoConnectTarget);
        App.ApplyIcon(_window.Handle);

        _window.Add(_foundLabel);
        _list.AddColumn(MainText.NameColumn, 170);
        _list.AddColumn(MainText.AddressColumn, 120);
        _list.AddColumn(MainText.StateColumn, 100);
        _list.FillLastColumn();
        _window.Add(_list);

        _window.Add(_addressLabel);
        _window.Add(_addressBox);
        _window.Add(_connect);
        _window.AcceptButton = _connect;
        _window.Add(_status);
        _window.Add(_options);

        _list.SelectedIndexChanged += () => _flow.OnSelectionChanged(_list.SelectedIndex);
        _list.ItemActivated += () => _flow.Activate(_list.SelectedIndex);
        _connect.Click += _flow.ConnectTyped;
        _options.Click += OpenOptions;
        _window.Closed += () =>
        {
            _flow.Shutdown();
            Closed?.Invoke();
        };
    }

    public string Address
    {
        get => _addressBox.Text;
        set => _addressBox.Text = value;
    }

    private void UpdateConnectionMode()
    {
        var network = _flow.Settings.Connection == ConnectionMode.Network;
        _addressLabel.Visible = _addressBox.Visible = network;
        _foundLabel.Text = network ? MainText.FoundLabel : MainText.FoundLabelUsb;
    }

    public void Start()
    {
        UpdateConnectionMode();
        _window.Show();
        _flow.Start();
    }

    public void Shutdown()
    {
        _flow.Shutdown();
        _window.Dispose();
    }

    public void Post(Action action) => App.Post(action);

    public void SetStatus(string text) => _status.Text = text;

    public void SetEnabled(bool enabled) => _connect.Enabled = _list.Enabled = enabled;

    public void ReplaceDevices(IReadOnlyList<SwitchDevice> devices, int selectIndex)
    {
        _list.Clear();
        foreach (var d in devices) _list.Add(new ListItem(MainText.DeviceCells(d)));
        _list.FillLastColumn();
        if (selectIndex >= 0) _list.SelectedIndex = selectIndex;
    }

    public void OpenViewer(ViewerSession session, IPEndPoint endpoint, string ffmpegPath, Action<string?> closed)
    {
        var done = false;
        void Finish(string? reason)
        {
            if (done) return;
            done = true;
            if (App.Quitting) return;
            UpdateConnectionMode();
            _window.Show();
            _window.Raise();
            closed(reason);
        }

        ViewerWindow? viewer = null;
        try
        {
            viewer = new ViewerWindow(session, _flow.Settings, endpoint, ffmpegPath, _media);
            _window.Hide();
            var opened = viewer;
            viewer.Closed += () => Finish(opened.EndReason);
            viewer.Start();
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            Log.Error("app", "viewer window failed", ex);
            if (viewer is null) session.Stop();
            else viewer.Close();
            Finish(ex.Message);
        }
    }

    public void AskUpdate(UpdateOffer offer, Action accepted)
    {
        if (App.Modal is not null)
        {
            Log.Write("update", "update dialog skipped: another dialog is open");
            return;
        }
        var dialog = new UpdateWindow(offer, _window.Handle);
        dialog.Accepted += accepted;
        dialog.Show();
    }

    public void Quit() => App.RequestQuit();

    private void OpenOptions()
    {
        if (App.Modal is not null) return;
        var options = new OptionsWindow(_flow.Settings, _window.Handle, _flow);
        options.Changed += UpdateConnectionMode;
        options.Closed += _flow.Settings.Save;
        options.Show();
    }
}
