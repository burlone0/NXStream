using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using NXStream.Core;
using NXStream.Render;

namespace NXStream.UI;

public sealed class VideoView : Control, IVideoSurface
{
    private GlRenderer? _gl;
    private volatile bool _fallback;
    private volatile bool _released;
    private string? _glProblem;
    private RenderSettings _settings = new(Scaler.Sharp, 0);

    private readonly object _lock = new();
    private Bitmap? _front;
    private Bitmap? _back;
    private int _repaintPending;

    private string[] _overlay = [];
    private string? _centerMessage = ViewerText.WaitingVideo;

    public VideoView()
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.Opaque, true);
        SetStyle(ControlStyles.OptimizedDoubleBuffer, false);
        BackColor = Color.Black;
        ForeColor = Color.White;
    }

    protected override CreateParams CreateParams
    {
        get
        {
            const int CS_OWNDC = 0x20, WS_CLIPSIBLINGS = 0x04000000, WS_CLIPCHILDREN = 0x02000000;
            var cp = base.CreateParams;
            cp.ClassStyle |= CS_OWNDC;
            cp.Style |= WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
            return cp;
        }
    }

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        if (_fallback || _released) return;
        if (_gl is null)
        {
            _gl = new GlRenderer();
            _gl.Failed += reason => BeginInvokeSafe(() => UseFallback(reason));
            _gl.Configure(_settings);
            PushOverlay();
            PushMessage();
        }
        _gl.SetViewSize(ClientSize);
        _gl.Attach(Handle);
    }

    protected override void OnHandleDestroyed(EventArgs e)
    {
        _gl?.Detach();
        base.OnHandleDestroyed(e);
    }

    public void ReleaseRenderer()
    {
        _released = true;
        var gl = _gl;
        _gl = null;
        gl?.Dispose();
    }

    private void UseFallback(string reason)
    {
        if (_fallback) return;
        Core.Log.Write("render", $"OpenGL unusable ({reason}), falling back to GDI+");
        _glProblem = reason;
        _fallback = true;
        var gl = _gl;
        _gl = null;
        Task.Run(() => gl?.Dispose());
        SetStyle(ControlStyles.OptimizedDoubleBuffer, true);
        Invalidate();
    }

    protected override void OnResize(EventArgs e)
    {
        base.OnResize(e);
        _gl?.SetViewSize(ClientSize);
        if (_fallback) Invalidate();
    }

    public void PresentFrame(ReadOnlySpan<byte> bgra, int width, int height)
    {
        if (_released) return;
        var gl = _gl;
        if (gl is not null && !_fallback)
        {
            gl.Present(bgra, width, height);
            return;
        }
        PresentGdi(bgra, width, height);
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
            return new RenderInfo("GDI+ (OpenGL unavailable)", "1280×720", "", 0, _glProblem);
        }
    }

    public async Task<Bitmap?> SnapshotAsync()
    {
        if (_gl is not { } gl || _fallback) return null;
        var (bgra, w, h) = await gl.SnapshotAsync();
        var bmp = new Bitmap(w, h, PixelFormat.Format32bppRgb);
        var data = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppRgb);
        for (var y = 0; y < h; y++)
            Marshal.Copy(bgra, (h - 1 - y) * w * 4, data.Scan0 + y * data.Stride, w * 4);
        bmp.UnlockBits(data);
        return bmp;
    }

    public void SetOverlay(string[] lines)
    {
        _overlay = lines;
        PushOverlay();
        if (_fallback) Invalidate();
    }

    public void ShowMessage(string? message)
    {
        lock (_lock) _centerMessage = message;
        PushMessage();
        if (_fallback) Invalidate();
    }

    private void PushOverlay() => _gl?.SetOverlay(_overlay.Length == 0 ? null : RenderOverlay(_overlay));

    private void PushMessage()
    {
        string? message;
        lock (_lock) message = _centerMessage;
        _gl?.SetMessage(message is null ? null : RenderMessage(message));
    }

    private static OverlayImage RenderOverlay(string[] lines)
    {
        var font = Theme.Mono;
        var lineHeight = TextRenderer.MeasureText("Ag", font).Height + 3;
        var width = lines.Max(l => TextRenderer.MeasureText(l, font).Width) + 28;
        var height = lineHeight * lines.Length + 18;
        return RenderText(width, height, 0.82f, false, g =>
        {
            for (var i = 0; i < lines.Length; i++)
            {
                var color = i == 0 ? Theme.Accent : lines[i].StartsWith('!') ? Theme.Warning : Theme.Text;
                TextRenderer.DrawText(g, lines[i], font, new Point(14, 9 + i * lineHeight), color, Color.Black);
            }
        });
    }

    private static OverlayImage RenderMessage(string message)
    {
        var lines = message.Split('\n');
        var title = TextRenderer.MeasureText(lines[0], Theme.Heading);
        var rest = lines.Skip(1).Select(l => TextRenderer.MeasureText(l, Theme.Body)).ToArray();
        var width = Math.Max(title.Width, rest.Length == 0 ? 0 : rest.Max(r => r.Width)) + 16;
        var height = title.Height + rest.Sum(r => r.Height + 4) + 16;
        return RenderText(width, height, 1f, true, g =>
        {
            TextRenderer.DrawText(g, lines[0], Theme.Heading, new Rectangle(0, 4, width, title.Height), Theme.Text,
                TextFormatFlags.HorizontalCenter);
            var y = 4 + title.Height + 8;
            for (var i = 1; i < lines.Length; i++)
            {
                TextRenderer.DrawText(g, lines[i], Theme.Body, new Rectangle(0, y, width, rest[i - 1].Height), Theme.Muted,
                    TextFormatFlags.HorizontalCenter);
                y += rest[i - 1].Height + 4;
            }
        });
    }

    private static OverlayImage RenderText(int width, int height, float opacity, bool centered, Action<Graphics> draw)
    {
        using var bmp = new Bitmap(width, height, PixelFormat.Format32bppRgb);
        using (var g = Graphics.FromImage(bmp))
        {
            g.Clear(Color.Black);
            draw(g);
        }
        var pixels = new byte[width * height * 4];
        var data = bmp.LockBits(new Rectangle(0, 0, width, height), ImageLockMode.ReadOnly, PixelFormat.Format32bppRgb);
        try
        {
            for (var y = 0; y < height; y++)
                Marshal.Copy(data.Scan0 + y * data.Stride, pixels, y * width * 4, width * 4);
        }
        finally
        {
            bmp.UnlockBits(data);
        }
        return new OverlayImage(pixels, width, height, centered, opacity);
    }

    protected override void OnPaintBackground(PaintEventArgs e)
    {
        if (_fallback) base.OnPaintBackground(e);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        if (!_fallback)
        {
            _gl?.Redraw();
            return;
        }
        PaintGdi(e.Graphics);
    }

    private void PresentGdi(ReadOnlySpan<byte> bgra, int width, int height)
    {
        lock (_lock)
        {
            if (_back is null || _back.Width != width || _back.Height != height)
            {
                _back?.Dispose();
                _back = new Bitmap(width, height, PixelFormat.Format32bppRgb);
            }
            var data = _back.LockBits(new Rectangle(0, 0, width, height), ImageLockMode.WriteOnly, PixelFormat.Format32bppRgb);
            try
            {
                var rowBytes = width * 4;
                unsafe
                {
                    for (var y = 0; y < height; y++)
                        bgra.Slice(y * rowBytes, rowBytes).CopyTo(new Span<byte>((byte*)(data.Scan0 + y * data.Stride), rowBytes));
                }
            }
            finally
            {
                _back.UnlockBits(data);
            }
            (_front, _back) = (_back, _front);
            _centerMessage = null;
        }
        if (Interlocked.Exchange(ref _repaintPending, 1) == 1) return;
        BeginInvokeSafe(() =>
        {
            Interlocked.Exchange(ref _repaintPending, 0);
            Invalidate();
        });
    }

    private void PaintGdi(Graphics g)
    {
        g.Clear(Color.Black);
        lock (_lock)
        {
            if (_front is not null)
            {
                var scale = Math.Min((float)ClientSize.Width / _front.Width, (float)ClientSize.Height / _front.Height);
                var w = (int)(_front.Width * scale);
                var h = (int)(_front.Height * scale);
                var dest = new Rectangle((ClientSize.Width - w) / 2, (ClientSize.Height - h) / 2, w, h);
                g.InterpolationMode = scale == 1f ? InterpolationMode.NearestNeighbor
                    : _settings.Scaler != Scaler.Smooth ? InterpolationMode.HighQualityBicubic : InterpolationMode.Bilinear;
                g.PixelOffsetMode = PixelOffsetMode.Half;
                g.CompositingMode = CompositingMode.SourceCopy;
                g.DrawImage(_front, dest);
                g.CompositingMode = CompositingMode.SourceOver;
            }
            if (_centerMessage is not null)
                TextRenderer.DrawText(g, _centerMessage, Theme.Heading, ClientRectangle, Theme.Muted,
                    TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
        }
        if (_overlay.Length == 0) return;

        var font = Theme.Body;
        var lineHeight = TextRenderer.MeasureText("Ag", font).Height + 2;
        var width = _overlay.Max(l => TextRenderer.MeasureText(l, font).Width) + 24;
        var box = new Rectangle(12, 12, width, lineHeight * _overlay.Length + 16);
        using (var brush = new SolidBrush(Color.FromArgb(170, 0, 0, 0))) g.FillRectangle(brush, box);
        for (var i = 0; i < _overlay.Length; i++)
            TextRenderer.DrawText(g, _overlay[i], font, new Point(box.X + 12, box.Y + 8 + i * lineHeight),
                i == 0 ? Theme.Accent : Theme.Text);
    }

    private void BeginInvokeSafe(Action action)
    {
        if (!IsHandleCreated || IsDisposed) return;
        try { BeginInvoke(action); } catch (InvalidOperationException) {  }
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _gl?.Dispose();
            _gl = null;
            lock (_lock)
            {
                _front?.Dispose();
                _back?.Dispose();
                _front = _back = null;
            }
        }
        base.Dispose(disposing);
    }
}
