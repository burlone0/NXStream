using System.Drawing;
using System.Globalization;
using NXStream.Core;

namespace NXStream.Linux.Ui;

internal sealed class UiWindow : IDisposable
{
    private const int CaretPhaseMs = 530;
    private static readonly List<UiWindow> Windows = [];
    private static readonly IntPtr[] Cursors = new IntPtr[3];

    private readonly IntPtr _window;
    private readonly IntPtr _renderer;
    private static readonly Lazy<(float Scale, string Source)> Desktop = new(DetectDesktopScale);
    private readonly float _windowScale = 1f;
    private readonly float _pixelRatio = 1f;
    private IntPtr _texture;
    private Canvas? _canvas;
    private Widget? _capture, _hover;
    private ulong _caretReset;
    private bool _lastPhase = true, _shown, _disposed;
    private readonly UiPalette _palette;
    private UiCursor _cursor = UiCursor.Arrow;

    public UiWindow(string title, int width, int height, UiPalette palette)
    {
        _palette = palette;
        var forcedScale = Environment.GetEnvironmentVariable("NXSTREAM_SCALE");
        if (float.TryParse(forcedScale, NumberStyles.Float, CultureInfo.InvariantCulture, out var forced) && forced >= 0.5f)
            _windowScale = forced;
        else if (Sdl.VideoDriver() != "wayland")
            _windowScale = Desktop.Value.Scale;
        _window = Sdl.SDL_CreateWindow(title, Sdl.WindowPosCentered, Sdl.WindowPosCentered,
            (int)Math.Round(width * _windowScale), (int)Math.Round(height * _windowScale), Sdl.WindowHidden | Sdl.WindowAllowHighDpi);
        if (_window == IntPtr.Zero) throw new InvalidOperationException("SDL_CreateWindow: " + Sdl.Error());
        _renderer = Sdl.SDL_CreateRenderer(_window, -1, Sdl.RendererSoftware);
        if (_renderer == IntPtr.Zero)
        {
            Sdl.SDL_DestroyWindow(_window);
            throw new InvalidOperationException("SDL_CreateRenderer(software): " + Sdl.Error());
        }
        Sdl.SDL_GetRendererOutputSize(_renderer, out var outW, out _);
        Sdl.SDL_GetWindowSize(_window, out var winW, out _);
        if (winW > 0) _pixelRatio = (float)outW / winW;
        WindowId = Sdl.SDL_GetWindowID(_window);
        Root = new Container { Owner = this, Bounds = new Rectangle(0, 0, width, height) };
        Windows.Add(this);
        Log.Write("ui", $"window '{title}' id {WindowId} {width}x{height} scale {Scale:0.##} " +
                        $"(window {_windowScale:0.##}, pixel ratio {_pixelRatio:0.##}) driver {Sdl.VideoDriver()}");
    }

    public event Action? Closed;

    public uint WindowId { get; }
    public IntPtr Handle => _window;
    public Container Root { get; }
    private static (float, string) DetectDesktopScale()
    {
        var scale = 1f;
        var source = "default";
        if (float.TryParse(Environment.GetEnvironmentVariable("GDK_SCALE"), NumberStyles.Integer, CultureInfo.InvariantCulture, out var gdk) && gdk >= 1)
            (scale, source) = (gdk, "GDK_SCALE");
        else
            try
            {
                using var xrdb = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("xrdb", "-query")
                    { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false });
                var output = xrdb!.StandardOutput.ReadToEndAsync();
                if (!xrdb.WaitForExit(1000)) xrdb.Kill();
                else
                {
                    var match = System.Text.RegularExpressions.Regex.Match(output.Result, @"^Xft\.dpi:\s*([\d.]+)", System.Text.RegularExpressions.RegexOptions.Multiline);
                    if (match.Success && float.TryParse(match.Groups[1].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out var dpi))
                        (scale, source) = (dpi / 96f, "Xft.dpi");
                }
            }
            catch (Exception) { }
        scale = MathF.Round(Math.Clamp(scale, 1f, 3f) * 4f) / 4f;
        Log.Write("ui", $"ui scale {scale:0.##} from {source}");
        return (scale, source);
    }

    public float Scale => _windowScale * _pixelRatio;
    public bool Dirty { get; private set; } = true;
    public bool Visible => _shown;
    public Widget? FocusedWidget { get; private set; }
    public bool KeyboardFocus { get; private set; }
    public Button? AcceptButton { get; set; }
    public Button? CancelButton { get; set; }
    public bool CaretOn => ((Sdl.SDL_GetTicks64() - _caretReset) / CaretPhaseMs) % 2 == 0;

    public T Add<T>(T widget) where T : Widget => Root.Add(widget);

    public void Invalidate() => Dirty = true;

    public void Show()
    {
        Sdl.SDL_ShowWindow(_window);
        _shown = true;
        Dirty = true;
        if (FocusedWidget is null) Focus(FirstTabStop());
    }

    public void Hide()
    {
        Sdl.SDL_HideWindow(_window);
        _shown = false;
    }

    public void Resize(int width, int height)
    {
        Root.Bounds = new Rectangle(0, 0, width, height);
        Sdl.SDL_SetWindowSize(_window, (int)Math.Round(width * _windowScale), (int)Math.Round(height * _windowScale));
        Invalidate();
    }

    public void Raise() => Sdl.SDL_RaiseWindow(_window);

    public void SetModalFor(IntPtr parent)
    {
        if (Sdl.SDL_SetWindowModalFor(_window, parent) != 0) Log.Write("ui", $"modal window {WindowId} failed: {Sdl.Error()}");
    }

    public void Focus(Widget? widget)
    {
        if (FocusedWidget == widget) return;
        var old = FocusedWidget;
        FocusedWidget = widget;
        ApplyTextInput();
        old?.FocusChanged();
        widget?.FocusChanged();
        ResetCaret();
        Invalidate();
    }

    private void ApplyTextInput()
    {
        if (FocusedWidget is TextBox) Sdl.SDL_StartTextInput();
        else Sdl.SDL_StopTextInput();
    }

    public void FocusNext(bool forward)
    {
        var stops = new List<Widget>();
        Root.CollectTabStops(stops);
        if (stops.Count == 0) return;
        var index = FocusedWidget is null ? -1 : stops.IndexOf(FocusedWidget);
        var next = forward ? (index + 1) % stops.Count : (index <= 0 ? stops.Count - 1 : index - 1);
        Focus(stops[next]);
    }

    private Widget? FirstTabStop()
    {
        var stops = new List<Widget>();
        Root.CollectTabStops(stops);
        return stops.Count > 0 ? stops[0] : null;
    }

    public void ResetCaret()
    {
        _caretReset = Sdl.SDL_GetTicks64();
        _lastPhase = true;
    }

    public static bool Dispatch(in SdlEvent ev)
    {
        foreach (var window in Windows.ToArray())
            if (window.HandleEvent(ev)) return true;
        return false;
    }

    public static void TickAll()
    {
        foreach (var window in Windows.ToArray())
        {
            window.Tick();
            window.Render();
        }
    }

    public void Tick()
    {
        if (FocusedWidget is not TextBox) return;
        var phase = CaretOn;
        if (phase == _lastPhase) return;
        _lastPhase = phase;
        Invalidate();
    }

    public bool HandleEvent(in SdlEvent ev)
    {
        if (ev.Type < Sdl.EventWindow || ev.Type > Sdl.EventMouseWheel || ev.WindowId != WindowId) return false;
        switch (ev.Type)
        {
            case Sdl.EventWindow:
                HandleWindowEvent(ev.WindowEventKind);
                break;
            case Sdl.EventKeyDown:
                HandleKey(ev.Sym, ev.Mod);
                break;
            case Sdl.EventTextInput:
                if (FocusedWidget is { Enabled: true } typing)
                {
                    typing.TextInput(ev.TextInput());
                    ResetCaret();
                }
                break;
            case Sdl.EventMouseMotion:
                HandleMove(ToDesign(ev.X), ToDesign(ev.Y));
                break;
            case Sdl.EventMouseButtonDown when ev.Button == 1:
                HandleDown(ToDesign(ev.X), ToDesign(ev.Y), ev.Clicks);
                break;
            case Sdl.EventMouseButtonUp when ev.Button == 1:
                HandleUp(ToDesign(ev.X), ToDesign(ev.Y));
                break;
            case Sdl.EventMouseWheel:
                if ((_hover ?? FocusedWidget) is { Enabled: true } target) target.MouseWheel(ev.WheelY);
                break;
        }
        return true;
    }

    private int ToDesign(int windowCoordinate) => (int)Math.Floor(windowCoordinate / _windowScale);

    private void HandleWindowEvent(byte kind)
    {
        switch (kind)
        {
            case Sdl.WindowEventShown:
            case Sdl.WindowEventExposed:
            case Sdl.WindowEventRestored:
            case Sdl.WindowEventSizeChanged:
                Invalidate();
                break;
            case Sdl.WindowEventLeave:
                SetHover(null);
                break;
            case Sdl.WindowEventFocusGained:
                ApplyTextInput();
                Invalidate();
                break;
            case Sdl.WindowEventFocusLost:
                if (_capture is not null) _capture.Pressed = false;
                _capture = null;
                Invalidate();
                break;
            case Sdl.WindowEventClose:
                Closed?.Invoke();
                break;
        }
    }

    private void HandleKey(int sym, int mod)
    {
        ResetCaret();
        if (sym == Keys.Tab)
        {
            KeyboardFocus = true;
            FocusNext((mod & Keys.ModShift) == 0);
            return;
        }
        if (FocusedWidget is { Enabled: true } focused && focused.KeyDown(sym, mod))
        {
            KeyboardFocus = true;
            Invalidate();
            return;
        }
        if (sym is Keys.Return or Keys.KpEnter && AcceptButton is { Enabled: true, Visible: true } accept) accept.PerformClick();
        else if (sym == Keys.Escape && CancelButton is { Enabled: true, Visible: true } cancel) cancel.PerformClick();
    }

    private void HandleMove(int x, int y)
    {
        var hit = Root.HitTest(x, y);
        SetHover(hit is { Interactive: true } ? hit : null);
        var target = _capture ?? _hover;
        if (target is { Enabled: true })
        {
            if (target == _capture) target.Hover = hit == target;
            target.MouseMove(x, y);
        }
        ApplyCursor();
    }

    private void HandleDown(int x, int y, int clicks)
    {
        var hit = Root.HitTest(x, y);
        if (hit is not { Interactive: true, Enabled: true }) return;
        KeyboardFocus = false;
        if (hit.Focusable) Focus(hit);
        _capture = hit;
        hit.Pressed = true;
        ResetCaret();
        hit.MouseDown(x, y, clicks);
        Invalidate();
    }

    private void HandleUp(int x, int y)
    {
        var capture = _capture;
        _capture = null;
        if (capture is null) return;
        capture.Pressed = false;
        capture.MouseUp(x, y);
        Invalidate();
        HandleMove(x, y);
    }

    private void SetHover(Widget? widget)
    {
        if (_hover == widget) return;
        if (_hover is not null) _hover.Hover = false;
        _hover = widget;
        if (widget is not null) widget.Hover = true;
        Invalidate();
    }

    private void ApplyCursor()
    {
        var wanted = _hover is { Enabled: true } ? _hover.Cursor : UiCursor.Arrow;
        if (wanted == _cursor) return;
        _cursor = wanted;
        var id = wanted switch { UiCursor.IBeam => Sdl.CursorIBeam, UiCursor.Hand => Sdl.CursorHand, _ => Sdl.CursorArrow };
        if (Cursors[(int)wanted] == IntPtr.Zero) Cursors[(int)wanted] = Sdl.SDL_CreateSystemCursor(id);
        Sdl.SDL_SetCursor(Cursors[(int)wanted]);
    }

    public unsafe void Render()
    {
        if (_disposed || !_shown || !Dirty) return;
        Dirty = false;
        Sdl.SDL_GetRendererOutputSize(_renderer, out var w, out var h);
        if (_canvas is null || _canvas.Width != w || _canvas.Height != h)
        {
            if (_texture != IntPtr.Zero) Sdl.SDL_DestroyTexture(_texture);
            _texture = Sdl.SDL_CreateTexture(_renderer, Sdl.PixelFormatArgb8888, Sdl.TextureAccessStreaming, w, h);
            _canvas = new Canvas(w, h);
        }
        _canvas.ResetClip();
        _canvas.Clear(_palette.Back);
        Root.Paint(new Gfx(_canvas, Scale, _palette));
        fixed (uint* pixels = _canvas.Pixels) Sdl.SDL_UpdateTexture(_texture, IntPtr.Zero, (byte*)pixels, w * 4);
        var dest = new SdlRect { X = 0, Y = 0, W = w, H = h };
        Sdl.SDL_RenderCopy(_renderer, _texture, IntPtr.Zero, ref dest);
        Sdl.SDL_RenderPresent(_renderer);
    }

    public unsafe byte[] ReadPixels(out int width, out int height)
    {
        Sdl.SDL_GetRendererOutputSize(_renderer, out width, out height);
        var bytes = new byte[width * height * 4];
        fixed (byte* p = bytes)
            if (Sdl.SDL_RenderReadPixels(_renderer, IntPtr.Zero, Sdl.PixelFormatArgb8888, p, width * 4) != 0)
                Log.Write("ui", "SDL_RenderReadPixels failed: " + Sdl.Error());
        return bytes;
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        Windows.Remove(this);
        if (_texture != IntPtr.Zero) Sdl.SDL_DestroyTexture(_texture);
        Sdl.SDL_DestroyRenderer(_renderer);
        Sdl.SDL_DestroyWindow(_window);
    }
}
