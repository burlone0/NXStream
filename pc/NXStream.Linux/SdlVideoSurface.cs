using System.Drawing;
using NXStream.Core;
using NXStream.Render;

namespace NXStream.Linux;

public sealed class SdlVideoSurface : IVideoSurface, IDisposable
{
    private readonly IntPtr _window;
    private readonly FrameSlot _slot = new();
    private GlRenderer? _gl;
    private volatile bool _fallback;
    private bool _rendererFailed;
    private string? _glProblem;
    private RenderSettings _settings = new(Scaler.Sharp, 0);
    private IntPtr _renderer, _texture;
    private int _textureWidth, _textureHeight;
    private volatile int _frameWidth = 1280, _frameHeight = 720;
    private OverlayImage? _overlay, _message, _menu;
    private readonly FallbackLayer _overlayLayer = new(), _messageLayer = new(), _menuLayer = new();

    public static void PrepareWindowAttributes()
    {
        Sdl.SDL_GL_SetAttribute(Sdl.GlContextMajorVersion, 3);
        Sdl.SDL_GL_SetAttribute(Sdl.GlContextMinorVersion, 3);
        Sdl.SDL_GL_SetAttribute(Sdl.GlContextProfileMask, Sdl.GlProfileCore);
        Sdl.SDL_GL_SetAttribute(Sdl.GlDoubleBuffer, 1);
    }

    public SdlVideoSurface(IntPtr window)
    {
        _window = window;
        _gl = new GlRenderer();
        _gl.Failed += UseFallback;
        _gl.Configure(_settings);
        OnWindowResized();
        _gl.Attach(window);
    }

    public void OnWindowResized()
    {
        Sdl.SDL_GL_GetDrawableSize(_window, out var w, out var h);
        _gl?.SetViewSize(new Size(w, h));
    }

    public void PresentFrame(ReadOnlySpan<byte> bgra, int width, int height)
    {
        var gl = _gl;
        if (gl is not null && !_fallback)
        {
            gl.Present(bgra, width, height);
            return;
        }
        _slot.Submit(bgra, width, height);
    }

    public void Configure(RenderSettings settings)
    {
        _settings = settings;
        _gl?.Configure(settings);
    }

    public RenderInfo Info
    {
        get
        {
            if (_gl is { } gl && !_fallback) return gl.Info;
            return new RenderInfo("SDL (OpenGL not available)", $"{_frameWidth}×{_frameHeight}", "", 0, _glProblem);
        }
    }

    public void SetOverlay(OverlayImage? overlay)
    {
        _overlay = overlay;
        _gl?.SetOverlay(overlay);
    }

    public void SetMessage(OverlayImage? message)
    {
        _message = message;
        _gl?.SetMessage(message);
    }

    public void SetMenu(OverlayImage? menu)
    {
        _menu = menu;
        _gl?.SetMenu(menu);
    }

    public void Redraw() => _gl?.Redraw();

    public Task<(byte[] Bgra, int Width, int Height)> SnapshotAsync() =>
        _gl is { } gl && !_fallback
            ? gl.SnapshotAsync()
            : Task.FromException<(byte[], int, int)>(new InvalidOperationException("OpenGL not active"));

    public bool PumpFallback()
    {
        if (!_fallback || _rendererFailed) return false;
        if (_renderer == IntPtr.Zero)
        {
            _renderer = Sdl.SDL_CreateRenderer(_window, -1, Sdl.RendererAccelerated | Sdl.RendererPresentVsync);
            if (_renderer == IntPtr.Zero) _renderer = Sdl.SDL_CreateRenderer(_window, -1, 0);
            if (_renderer == IntPtr.Zero)
            {
                _rendererFailed = true;
                Log.Write("render", "SDL renderer fallback failed: " + Sdl.Error());
                return false;
            }
            Log.Write("render", "SDL renderer fallback created");
        }

        var fresh = _slot.Take(out var frame, out var width, out var height);
        if (width <= 0 || height <= 0) return false;
        if (fresh)
        {
            if (_texture == IntPtr.Zero || width != _textureWidth || height != _textureHeight)
            {
                if (_texture != IntPtr.Zero) Sdl.SDL_DestroyTexture(_texture);
                _texture = Sdl.SDL_CreateTexture(_renderer, Sdl.PixelFormatArgb8888, Sdl.TextureAccessStreaming, width, height);
                _textureWidth = width;
                _textureHeight = height;
            }
            if (_texture != IntPtr.Zero)
                unsafe { fixed (byte* p = frame) Sdl.SDL_UpdateTexture(_texture, IntPtr.Zero, p, width * 4); }
            _frameWidth = width;
            _frameHeight = height;
            _message = null;
        }

        Sdl.SDL_SetRenderDrawColor(_renderer, 0, 0, 0, 255);
        Sdl.SDL_RenderClear(_renderer);
        if (_texture != IntPtr.Zero && Sdl.SDL_GetRendererOutputSize(_renderer, out var w, out var h) == 0 && w > 0 && h > 0)
        {
            var scale = Math.Min(w / (double)width, h / (double)height);
            var dw = Math.Max(1, (int)Math.Round(width * scale));
            var dh = Math.Max(1, (int)Math.Round(height * scale));
            var dest = new SdlRect { X = (w - dw) / 2, Y = (h - dh) / 2, W = dw, H = dh };
            Sdl.SDL_RenderCopy(_renderer, _texture, IntPtr.Zero, ref dest);
        }
        if (Sdl.SDL_GetRendererOutputSize(_renderer, out var ow, out var oh) == 0)
        {
            DrawLayer(_messageLayer, _message, ow, oh);
            DrawLayer(_overlayLayer, _overlay, ow, oh);
            DrawLayer(_menuLayer, _menu, ow, oh);
        }
        Sdl.SDL_RenderPresent(_renderer);
        return true;
    }

    private unsafe void DrawLayer(FallbackLayer layer, OverlayImage? image, int viewWidth, int viewHeight)
    {
        if (image is null) return;
        if (!ReferenceEquals(layer.Uploaded, image))
        {
            if (layer.Texture == IntPtr.Zero || layer.Width != image.Width || layer.Height != image.Height)
            {
                if (layer.Texture != IntPtr.Zero) Sdl.SDL_DestroyTexture(layer.Texture);
                layer.Texture = Sdl.SDL_CreateTexture(_renderer, Sdl.PixelFormatArgb8888, Sdl.TextureAccessStreaming,
                                                      image.Width, image.Height);
                layer.Width = image.Width;
                layer.Height = image.Height;
                if (layer.Texture != IntPtr.Zero) Sdl.SDL_SetTextureBlendMode(layer.Texture, Sdl.BlendModeBlend);
            }
            if (layer.Texture == IntPtr.Zero) return;
            fixed (byte* p = image.Pixels) Sdl.SDL_UpdateTexture(layer.Texture, IntPtr.Zero, p, image.Width * 4);
            layer.Uploaded = image;
        }
        Sdl.SDL_SetTextureAlphaMod(layer.Texture, (byte)Math.Round(image.Opacity * 255));
        var dest = new SdlRect
        {
            X = image.Centered ? (viewWidth - image.Width) / 2 : image.X >= 0 ? image.X : 12,
            Y = image.Centered ? (viewHeight - image.Height) / 2 : image.Y >= 0 ? image.Y : 12,
            W = image.Width,
            H = image.Height,
        };
        Sdl.SDL_RenderCopy(_renderer, layer.Texture, IntPtr.Zero, ref dest);
    }

    private sealed class FallbackLayer
    {
        public IntPtr Texture;
        public OverlayImage? Uploaded;
        public int Width, Height;
    }

    private void UseFallback(string reason)
    {
        if (_fallback) return;
        Log.Write("render", $"OpenGL unusable ({reason}), falling back to the SDL renderer");
        _glProblem = reason;
        _fallback = true;
        var gl = _gl;
        _gl = null;
        Task.Run(() => gl?.Dispose());
    }

    public void Dispose()
    {
        var gl = _gl;
        _gl = null;
        if (gl is not null)
        {
            gl.Detach();
            gl.Dispose();
        }
        foreach (var layer in new[] { _overlayLayer, _messageLayer, _menuLayer })
            if (layer.Texture != IntPtr.Zero) Sdl.SDL_DestroyTexture(layer.Texture);
        if (_texture != IntPtr.Zero) Sdl.SDL_DestroyTexture(_texture);
        if (_renderer != IntPtr.Zero) Sdl.SDL_DestroyRenderer(_renderer);
        _texture = _renderer = IntPtr.Zero;
    }
}
