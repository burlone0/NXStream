using System.Net;
using System.Runtime.InteropServices;
using NXStream.Core;
using NXStream.Media;

namespace NXStream.UI;

public sealed class ViewerForm : Form, IViewerHost
{
    private const uint EsContinuous = 0x80000000, EsSystemRequired = 0x1, EsDisplayRequired = 0x2;

    private readonly AppSettings _settings;
    private readonly VideoView _view = new() { Dock = DockStyle.Fill };
    private readonly System.Windows.Forms.Timer _hintTimer = new() { Interval = ViewerText.HintMs };
    private readonly ViewerController _controller;

    private bool _fullscreen;
    private bool _fullscreenSetting;
    private FormBorderStyle _savedBorder;
    private FormWindowState _savedState;
    private Rectangle _savedBounds;

    public string? EndReason => _controller.EndReason;

    public ViewerForm(ViewerSession session, AppSettings settings, IPEndPoint endpoint, string ffmpegPath)
    {
        _settings = settings;

        Text = $"NXStream · {session.Console.Name}";
        BackColor = Color.Black;
        KeyPreview = true;
        StartPosition = FormStartPosition.CenterScreen;
        MinimumSize = new Size(480, 300);
        try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); }
        catch (ArgumentException) { }

        var area = Screen.FromPoint(Cursor.Position).WorkingArea;
        var width = Math.Min(1280, (int)(area.Width * 0.8));
        ClientSize = new Size(width, width * 9 / 16);

        Controls.Add(_view);
        _view.ContextMenuStrip = BuildMenu();
        _view.DoubleClick += (_, _) => ToggleFullscreen();
        _controller = new ViewerController(this, _view, new WindowsMedia(), settings, session, endpoint, ffmpegPath);
        _hintTimer.Tick += (_, _) => { _hintTimer.Stop(); _controller.HideHint(); };
        WindowsTheme.Apply(this);
        BackColor = Color.Black;
    }

    private ContextMenuStrip BuildMenu()
    {
        var menu = new ContextMenuStrip { ShowImageMargin = false };
        WindowsTheme.Apply(menu);
        menu.Items.Add($"{ViewerText.Fullscreen}\t{ViewerText.KeyFullscreen}", null, (_, _) => ToggleFullscreen());
        menu.Items.Add($"{ViewerText.Stats}\t{ViewerText.KeyStats}", null, (_, _) => _controller.ToggleStats());
        menu.Items.Add(ViewerText.Options, null, (_, _) => ShowOptions());
        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add(ViewerText.Disconnect, null, (_, _) => Close());
        return menu;
    }

    protected override void OnShown(EventArgs e)
    {
        base.OnShown(e);
        _hintTimer.Start();
        _fullscreenSetting = _settings.StartFullscreen;
        if (_settings.StartFullscreen) ToggleFullscreen();
        _controller.Start();
    }

    protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
    {
        switch (keyData)
        {
            case Keys.F11:
            case Keys.Alt | Keys.Enter:
                ToggleFullscreen();
                return true;
            case Keys.Escape when _fullscreen:
                ToggleFullscreen();
                return true;
            case Keys.F3:
                _controller.ToggleStats();
                return true;
        }
        return base.ProcessCmdKey(ref msg, keyData);
    }

    private void ToggleFullscreen()
    {
        if (!_fullscreen)
        {
            _savedBorder = FormBorderStyle;
            _savedState = WindowState;
            _savedBounds = Bounds;
            FormBorderStyle = FormBorderStyle.None;
            WindowState = FormWindowState.Normal;
            Bounds = Screen.FromControl(this).Bounds;
        }
        else
        {
            FormBorderStyle = _savedBorder;
            Bounds = _savedBounds;
            WindowState = _savedState;
        }
        _fullscreen = !_fullscreen;
        Log.Write("ui", $"{(_fullscreen ? "fullscreen" : "windowed")} bounds {Bounds.Width}x{Bounds.Height}, " +
                        $"video area {_view.ClientSize.Width}x{_view.ClientSize.Height}");
    }

    private void ShowOptions()
    {
        using var options = new OptionsForm(_settings);
        options.Changed += ApplySettings;
        options.ShowDialog(this);
        _settings.Save();
    }

    private void ApplySettings()
    {
        _controller.ApplySettings();
        if (_settings.StartFullscreen != _fullscreenSetting)
        {
            _fullscreenSetting = _settings.StartFullscreen;
            if (_fullscreen != _fullscreenSetting) ToggleFullscreen();
        }
    }

    void IViewerHost.Post(Action action)
    {
        if (IsDisposed || !IsHandleCreated) return;
        try { BeginInvoke(() => { if (!IsDisposed) action(); }); }
        catch (InvalidOperationException) { }
    }

    void IViewerHost.ShowMessage(string title, string body) => _view.ShowMessage($"{title}\n{body}");

    void IViewerHost.ShowOverlay(string[] lines) => _view.SetOverlay(lines);

    void IViewerHost.ApplyKeepAwake(bool on) =>
        SetThreadExecutionState(on ? EsContinuous | EsSystemRequired | EsDisplayRequired : EsContinuous);

    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        _hintTimer.Dispose();
        _controller.Stop();
        base.OnFormClosing(e);
    }

    protected override void OnFormClosed(FormClosedEventArgs e)
    {
        _view.ReleaseRenderer();
        base.OnFormClosed(e);
    }

    [DllImport("kernel32.dll")]
    private static extern uint SetThreadExecutionState(uint flags);
}
