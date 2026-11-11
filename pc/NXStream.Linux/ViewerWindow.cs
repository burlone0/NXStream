using System.Drawing;
using System.Net;
using System.Runtime.InteropServices;
using NXStream.Core;
using NXStream.Linux.Ui;
using NXStream.Media;
using NXStream.Render;

namespace NXStream.Linux;

internal sealed class ViewerWindow : IViewerHost
{
    private const int AltMask = 0x0300, KeyMenu = 0x40000065, KeyF10 = 0x40000043;

    private readonly AppSettings _settings;
    private readonly IntPtr _window;
    private readonly SdlVideoSurface _view;
    private readonly uint _id;
    private readonly ulong _hintUntil = Sdl.SDL_GetTicks64() + ViewerText.HintMs;
    private readonly ViewerController _controller;
    private OptionsWindow? _options;
    private MenuModel? _menu;
    private Point _menuAt;
    private Point? _pointer;
    private float _scale = 1f;
    private bool _showHint = true;
    private bool _closed;
    private bool _fullscreen;
    private bool _fullscreenSetting;

    public string? EndReason => _controller.EndReason;
    public uint WindowId => _id;

    public event Action? Closed;

    public ViewerWindow(ViewerSession session, AppSettings settings, IPEndPoint endpoint, string ffmpegPath, IMediaFactory media)
    {
        _settings = settings;

        var (display, area) = CursorDisplay();
        var width = Math.Min(1280, (int)(area.W * 0.8));
        SdlVideoSurface.PrepareWindowAttributes();
        _window = Sdl.SDL_CreateWindow($"NXStream · {session.Console.Name}", Sdl.WindowPosCentered | display,
                                       Sdl.WindowPosCentered | display, width, width * 9 / 16,
                                       Sdl.WindowShown | Sdl.WindowResizable | Sdl.WindowOpenGl | Sdl.WindowAllowHighDpi);
        if (_window == IntPtr.Zero) throw new InvalidOperationException("Window not created: " + Sdl.Error());
        Sdl.SDL_SetWindowMinimumSize(_window, 480, 300);
        App.ApplyIcon(_window);
        _id = Sdl.SDL_GetWindowID(_window);
        UpdateScale();
        try
        {
            _view = new SdlVideoSurface(_window);
        }
        catch
        {
            Sdl.SDL_DestroyWindow(_window);
            throw;
        }
        _view.SetMessage(Message(ViewerText.WaitingVideo, ""));
        _controller = new ViewerController(this, _view, media, settings, session, endpoint, ffmpegPath);
        App.Viewer = this;
    }

    public void Start()
    {
        _fullscreenSetting = _settings.StartFullscreen;
        if (_settings.StartFullscreen) ToggleFullscreen();
        _controller.Start();
    }

    public bool HandleEvent(in SdlEvent ev)
    {
        if (_closed || ev.WindowId != _id) return false;
        switch (ev.Type)
        {
            case Sdl.EventWindow:
                HandleWindowEvent(ev.WindowEventKind);
                break;
            case Sdl.EventKeyDown:
                HandleKey(ev.Sym, ev.Mod);
                break;
            case Sdl.EventMouseMotion:
                HandleMotion(ev.X, ev.Y);
                break;
            case Sdl.EventMouseButtonDown:
                HandleButton(ev.Button, ev.Clicks, ev.X, ev.Y);
                break;
        }
        return true;
    }

    private void HandleWindowEvent(byte kind)
    {
        switch (kind)
        {
            case Sdl.WindowEventSizeChanged:
                SyncFullscreen();
                UpdateScale();
                _view.OnWindowResized();
                break;
            case Sdl.WindowEventExposed:
                _view.Redraw();
                break;
            case Sdl.WindowEventFocusLost:
                CloseMenu();
                break;
            case Sdl.WindowEventClose:
                Close();
                break;
        }
    }

    private void HandleKey(int sym, int mod)
    {
        if (_menu is not null && HandleMenuKey(sym)) return;
        if (sym == Sdl.KeyF11 || (sym is Keys.Return or Keys.KpEnter && (mod & AltMask) != 0)) ToggleFullscreen();
        else if (sym == Keys.Escape && _fullscreen) ToggleFullscreen();
        else if (sym == Sdl.KeyF3) _controller.ToggleStats();
        else if (sym == KeyMenu || (sym == KeyF10 && (mod & Keys.ModShift) != 0)) OpenMenuAtPointer();
    }

    private bool HandleMenuKey(int sym)
    {
        var menu = _menu!;
        switch (sym)
        {
            case Keys.Escape:
                CloseMenu();
                return true;
            case Keys.Down:
            case Keys.Up:
                menu.Move(sym == Keys.Down ? 1 : -1);
                PushMenu();
                return true;
            case Keys.Return or Keys.KpEnter or Keys.Space:
                if (menu.HoverIndex < 0) return true;
                CloseMenu();
                menu.Activate(menu.HoverIndex);
                return true;
        }
        return false;
    }

    private void HandleMotion(int x, int y)
    {
        _pointer = new Point((int)(x * _scale), (int)(y * _scale));
        if (_menu is null) return;
        if (_menu.Hover((int)(x * _scale) - _menuAt.X, (int)(y * _scale) - _menuAt.Y)) PushMenu();
    }

    private void HandleButton(int button, int clicks, int x, int y)
    {
        var px = (int)(x * _scale);
        var py = (int)(y * _scale);
        _pointer = new Point(px, py);
        if (button == 3)
        {
            OpenMenu(px, py);
            return;
        }
        if (button != 1) return;
        if (_menu is not null)
        {
            var index = _menu.HitTest(px - _menuAt.X, py - _menuAt.Y);
            var menu = _menu;
            CloseMenu();
            menu.Activate(index);
            return;
        }
        if (clicks == 2) ToggleFullscreen();
    }

    private void OpenMenuAtPointer()
    {
        if (_pointer is { } pointer)
        {
            OpenMenu(pointer.X, pointer.Y);
            return;
        }
        Sdl.SDL_GL_GetDrawableSize(_window, out var width, out var height);
        OpenMenu(width / 2, height / 2);
    }

    private void OpenMenu(int px, int py)
    {
        var menu = new MenuModel();
        menu.Add(ViewerText.Fullscreen, ViewerText.KeyFullscreen, ToggleFullscreen);
        menu.Add(ViewerText.Stats, ViewerText.KeyStats, _controller.ToggleStats);
        menu.Add(ViewerText.Options, null, ShowOptions);
        menu.AddSeparator();
        menu.Add(ViewerText.Disconnect, null, Close);
        var image = menu.RenderToBgra(_scale, App.Palette);
        Sdl.SDL_GL_GetDrawableSize(_window, out var width, out var height);
        _menu = menu;
        _menuAt = new Point(Math.Max(0, Math.Min(px, width - image.Width)), Math.Max(0, Math.Min(py, height - image.Height)));
        PushMenu();
    }

    private void PushMenu()
    {
        if (_menu is null) return;
        _view.SetMenu(_menu.RenderToBgra(_scale, App.Palette) with { X = _menuAt.X, Y = _menuAt.Y });
    }

    private void CloseMenu()
    {
        if (_menu is null) return;
        _menu = null;
        _view.SetMenu(null);
    }

    private void ToggleFullscreen()
    {
        var wanted = !_fullscreen;
        if (Sdl.SDL_SetWindowFullscreen(_window, wanted ? Sdl.WindowFullscreenDesktop : 0) == 0)
        {
            _fullscreen = wanted;
            LogFullscreen();
        }
        else Log.Write("ui", $"{(wanted ? "fullscreen" : "windowed")} failed: {Sdl.Error()}");
    }

    private void SyncFullscreen()
    {
        var fullscreen = (Sdl.SDL_GetWindowFlags(_window) & Sdl.WindowFullscreenDesktop) != 0;
        if (fullscreen == _fullscreen) return;
        _fullscreen = fullscreen;
        LogFullscreen();
    }

    private void LogFullscreen()
    {
        Sdl.SDL_GL_GetDrawableSize(_window, out var width, out var height);
        Log.Write("ui", $"{(_fullscreen ? "fullscreen" : "windowed")}, video area {width}x{height}");
    }

    private void ShowOptions()
    {
        if (_options is not null || App.Modal is not null) return;
        _options = new OptionsWindow(_settings, _window);
        _options.Changed += ApplySettings;
        _options.Closed += () =>
        {
            _options = null;
            _settings.Save();
        };
        _options.Show();
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

    void IViewerHost.Post(Action action) => App.Post(action);

    void IViewerHost.ShowMessage(string title, string body) => _view.SetMessage(Message(title, body));

    void IViewerHost.ShowOverlay(string[] lines) =>
        _view.SetOverlay(lines.Length == 0 ? null : TextRenderer.RasterizeStats(lines, _scale));

    void IViewerHost.ApplyKeepAwake(bool on)
    {
        if (on) Sdl.SDL_DisableScreenSaver();
        else Sdl.SDL_EnableScreenSaver();
    }

    private OverlayImage Message(string title, string body) => TextRenderer.RasterizeMessage(title, body, _scale);

    private void UpdateScale()
    {
        Sdl.SDL_GL_GetDrawableSize(_window, out var drawable, out _);
        Sdl.SDL_GetWindowSize(_window, out var logical, out _);
        var scale = logical > 0 ? (float)drawable / logical : 1f;
        if (Math.Abs(scale - _scale) < 0.01f) return;
        _scale = scale;
        if (_controller is not null) _controller.RefreshOverlay();
    }

    public void Tick()
    {
        if (_closed) return;
        if (_showHint && Sdl.SDL_GetTicks64() >= _hintUntil)
        {
            _showHint = false;
            _controller.HideHint();
        }
        _view.PumpFallback();
    }

    public async Task<(byte[] Bgra, int Width, int Height)> SnapshotAsync() => await _view.SnapshotAsync();

    public void Close()
    {
        if (_closed) return;
        _closed = true;
        _options?.Close();
        _controller.Stop();
        if (App.Viewer == this) App.Viewer = null;
        _view.Dispose();
        Sdl.SDL_DestroyWindow(_window);
        Closed?.Invoke();
    }

    private static (int Display, SdlRect Area) CursorDisplay()
    {
        Sdl.SDL_GetGlobalMouseState(out var mx, out var my);
        var count = Math.Max(1, Sdl.SDL_GetNumVideoDisplays());
        var pick = 0;
        for (var i = 0; i < count; i++)
        {
            if (Sdl.SDL_GetDisplayBounds(i, out var bounds) != 0) continue;
            if (mx < bounds.X || my < bounds.Y || mx >= bounds.X + bounds.W || my >= bounds.Y + bounds.H) continue;
            pick = i;
            break;
        }
        if (Sdl.SDL_GetDisplayUsableBounds(pick, out var usable) != 0) usable = new SdlRect { W = 1600, H = 900 };
        return (pick, usable);
    }
}

internal static partial class Sdl
{
    public const int BlendModeBlend = 1;

    [DllImport("libSDL2-2.0.so.0")] public static extern void SDL_SetWindowMinimumSize(IntPtr window, int w, int h);
    [DllImport("libSDL2-2.0.so.0")] public static extern uint SDL_GetGlobalMouseState(out int x, out int y);
    [DllImport("libSDL2-2.0.so.0")] public static extern int SDL_GetNumVideoDisplays();
    [DllImport("libSDL2-2.0.so.0")] public static extern int SDL_GetDisplayBounds(int index, out SdlRect rect);
    [DllImport("libSDL2-2.0.so.0")] public static extern int SDL_GetDisplayUsableBounds(int index, out SdlRect rect);
    [DllImport("libSDL2-2.0.so.0")] public static extern int SDL_SetTextureBlendMode(IntPtr texture, int mode);
    [DllImport("libSDL2-2.0.so.0")] public static extern int SDL_SetTextureAlphaMod(IntPtr texture, byte alpha);
    [DllImport("libSDL2-2.0.so.0")] public static extern int SDL_PushEvent(ref SdlEvent ev);
}
