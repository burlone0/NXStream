using System.Drawing;

namespace NXStream.Linux.Ui;

internal sealed class Canvas
{
    private int _clipL, _clipT, _clipR, _clipB;

    public Canvas(int width, int height)
    {
        Width = width;
        Height = height;
        Pixels = new uint[width * height];
        ResetClip();
    }

    public int Width { get; }
    public int Height { get; }
    public uint[] Pixels { get; }

    public void ResetClip()
    {
        _clipL = _clipT = 0;
        _clipR = Width;
        _clipB = Height;
    }

    public Rectangle SetClip(Rectangle r)
    {
        var old = Rectangle.FromLTRB(_clipL, _clipT, _clipR, _clipB);
        _clipL = Math.Max(0, r.Left);
        _clipT = Math.Max(0, r.Top);
        _clipR = Math.Min(Width, r.Right);
        _clipB = Math.Min(Height, r.Bottom);
        return old;
    }

    public void Clear(Color c) => Array.Fill(Pixels, Pack(c));

    public byte[] ToBgra()
    {
        var bytes = new byte[Pixels.Length * 4];
        Buffer.BlockCopy(Pixels, 0, bytes, 0, bytes.Length);
        return bytes;
    }

    public static uint Pack(Color c) => 0xFF000000u | (uint)(c.R << 16) | (uint)(c.G << 8) | c.B;

    private void Blend(int x, int y, Color c, int alpha)
    {
        if (x < _clipL || x >= _clipR || y < _clipT || y >= _clipB || alpha <= 0) return;
        ref var px = ref Pixels[y * Width + x];
        if (alpha >= 255)
        {
            px = Pack(c);
            return;
        }
        var inv = 255 - alpha;
        var r = (c.R * alpha + (int)((px >> 16) & 255) * inv + 127) / 255;
        var g = (c.G * alpha + (int)((px >> 8) & 255) * inv + 127) / 255;
        var b = (c.B * alpha + (int)(px & 255) * inv + 127) / 255;
        px = 0xFF000000u | (uint)(r << 16) | (uint)(g << 8) | (uint)b;
    }

    public void FillRect(Rectangle r, Color c)
    {
        var x0 = Math.Max(r.Left, _clipL);
        var x1 = Math.Min(r.Right, _clipR);
        var y0 = Math.Max(r.Top, _clipT);
        var y1 = Math.Min(r.Bottom, _clipB);
        for (var y = y0; y < y1; y++)
            for (var x = x0; x < x1; x++)
                Blend(x, y, c, c.A);
    }

    public void StrokeRect(Rectangle r, Color c, int t = 1)
    {
        FillRect(new Rectangle(r.X, r.Y, r.Width, t), c);
        FillRect(new Rectangle(r.X, r.Bottom - t, r.Width, t), c);
        FillRect(new Rectangle(r.X, r.Y + t, t, r.Height - 2 * t), c);
        FillRect(new Rectangle(r.Right - t, r.Y + t, t, r.Height - 2 * t), c);
    }

    public void FillRoundRect(RectangleF r, float radius, Color c) => Shape(r, radius, 0, c);

    public void StrokeRoundRect(RectangleF r, float radius, float thickness, Color c) => Shape(r, radius, thickness, c);

    public void FillCircle(RectangleF r, Color c) => Shape(r, Math.Min(r.Width, r.Height) / 2, 0, c);

    public void StrokeCircle(RectangleF r, float thickness, Color c) => Shape(r, Math.Min(r.Width, r.Height) / 2, thickness, c);

    private void Shape(RectangleF r, float radius, float thickness, Color c)
    {
        radius = Math.Min(radius, Math.Min(r.Width, r.Height) / 2);
        var cx = r.X + r.Width / 2;
        var cy = r.Y + r.Height / 2;
        var hx = r.Width / 2 - radius;
        var hy = r.Height / 2 - radius;
        var x0 = Math.Max((int)Math.Floor(r.X) - 1, _clipL);
        var x1 = Math.Min((int)Math.Ceiling(r.Right) + 1, _clipR);
        var y0 = Math.Max((int)Math.Floor(r.Y) - 1, _clipT);
        var y1 = Math.Min((int)Math.Ceiling(r.Bottom) + 1, _clipB);
        for (var y = y0; y < y1; y++)
        {
            for (var x = x0; x < x1; x++)
            {
                var qx = Math.Abs(x + 0.5f - cx) - hx;
                var qy = Math.Abs(y + 0.5f - cy) - hy;
                var inner = Math.Max(qx, qy) - radius;
                if (thickness > 0 && inner < -(thickness + 1)) continue;
                var outside = MathF.Sqrt(Math.Max(qx, 0) * Math.Max(qx, 0) + Math.Max(qy, 0) * Math.Max(qy, 0));
                var d = outside + Math.Min(Math.Max(qx, qy), 0) - radius;
                var cover = Math.Clamp(0.5f - d, 0f, 1f);
                if (thickness > 0) cover *= Math.Clamp(d + thickness + 0.5f, 0f, 1f);
                Blend(x, y, c, (int)(cover * c.A + 0.5f));
            }
        }
    }

    public void Line(float x1, float y1, float x2, float y2, float thickness, Color c)
    {
        var pad = thickness / 2 + 1;
        var xa = Math.Max((int)Math.Floor(Math.Min(x1, x2) - pad), _clipL);
        var xb = Math.Min((int)Math.Ceiling(Math.Max(x1, x2) + pad), _clipR);
        var ya = Math.Max((int)Math.Floor(Math.Min(y1, y2) - pad), _clipT);
        var yb = Math.Min((int)Math.Ceiling(Math.Max(y1, y2) + pad), _clipB);
        var dx = x2 - x1;
        var dy = y2 - y1;
        var len2 = dx * dx + dy * dy;
        for (var y = ya; y < yb; y++)
        {
            for (var x = xa; x < xb; x++)
            {
                var px = x + 0.5f - x1;
                var py = y + 0.5f - y1;
                var t = len2 == 0 ? 0 : Math.Clamp((px * dx + py * dy) / len2, 0f, 1f);
                var ex = px - t * dx;
                var ey = py - t * dy;
                var dist = MathF.Sqrt(ex * ex + ey * ey);
                Blend(x, y, c, (int)(Math.Clamp(thickness / 2 + 0.5f - dist, 0f, 1f) * c.A + 0.5f));
            }
        }
    }

    public void BlitAlpha(byte[] alpha, int w, int h, int x, int y, Color c)
    {
        for (var j = 0; j < h; j++)
        {
            var py = y + j;
            if (py < _clipT || py >= _clipB) continue;
            for (var i = 0; i < w; i++)
            {
                var a = alpha[j * w + i];
                if (a != 0) Blend(x + i, py, c, a * c.A / 255);
            }
        }
    }
}
