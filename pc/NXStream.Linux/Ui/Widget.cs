using System.Drawing;

namespace NXStream.Linux.Ui;

internal enum UiCursor { Arrow, IBeam, Hand }

internal static class Keys
{
    public const int Backspace = 8, Tab = 9, Return = 13, Escape = 27, Space = 32, Delete = 127;
    public const int Right = 0x4000004F, Left = 0x40000050, Down = 0x40000051, Up = 0x40000052;
    public const int Home = 0x4000004A, End = 0x4000004D, PageUp = 0x4000004B, PageDown = 0x4000004E, KpEnter = 0x40000058;
    public const int ModShift = 0x0003, ModCtrl = 0x00C0;
}

internal sealed class Gfx
{
    public Gfx(Canvas canvas, float scale, UiPalette palette)
    {
        Canvas = canvas;
        Scale = scale;
        Palette = palette;
    }

    public Canvas Canvas { get; }
    public float Scale { get; }
    public UiPalette Palette { get; }

    public int S(float v) => (int)Math.Round(v * Scale);

    public Rectangle Px(Rectangle r)
    {
        var x0 = (int)Math.Round(r.Left * Scale);
        var y0 = (int)Math.Round(r.Top * Scale);
        return Rectangle.FromLTRB(x0, y0, (int)Math.Round(r.Right * Scale), (int)Math.Round(r.Bottom * Scale));
    }

    public int TextWidth(string text, UiFont font) => TextRenderer.Width(text, font, Scale);

    public int TextHeight(UiFont font) => TextRenderer.Height(font, Scale);

    public void Text(string text, UiFont font, int x, int y, Color color) => TextRenderer.Draw(Canvas, text, font, Scale, x, y, color);

    public void DottedRect(Rectangle r, Color c)
    {
        for (var x = r.Left; x < r.Right; x += 2)
        {
            Canvas.FillRect(new Rectangle(x, r.Top, 1, 1), c);
            Canvas.FillRect(new Rectangle(x, r.Bottom - 1, 1, 1), c);
        }
        for (var y = r.Top; y < r.Bottom; y += 2)
        {
            Canvas.FillRect(new Rectangle(r.Left, y, 1, 1), c);
            Canvas.FillRect(new Rectangle(r.Right - 1, y, 1, 1), c);
        }
    }

    public void TextCentered(string text, UiFont font, Rectangle px, Color color, bool horizontal = true)
    {
        var x = horizontal ? px.X + (px.Width - TextWidth(text, font)) / 2 : px.X;
        Text(text, font, x, px.Y + (px.Height - TextHeight(font)) / 2, color);
    }
}

internal abstract class Widget
{
    private Rectangle _bounds;
    private bool _visible = true, _enabled = true;

    public Rectangle Bounds
    {
        get => _bounds;
        set
        {
            _bounds = value;
            Invalidate();
        }
    }

    public Point Location
    {
        get => _bounds.Location;
        set => Bounds = new Rectangle(value, _bounds.Size);
    }

    public Size Size
    {
        get => _bounds.Size;
        set => Bounds = new Rectangle(_bounds.Location, value);
    }

    public bool Visible
    {
        get => _visible;
        set
        {
            _visible = value;
            Invalidate();
        }
    }

    public bool Enabled
    {
        get => _enabled;
        set
        {
            if (_enabled == value) return;
            _enabled = value;
            Invalidate();
        }
    }

    public Container? Parent { get; internal set; }
    public virtual bool Interactive => false;
    public virtual bool TabStop => false;
    public virtual bool Focusable => TabStop;
    public virtual UiCursor Cursor => UiCursor.Arrow;
    public bool Hover { get; internal set; }
    public bool Pressed { get; internal set; }
    public bool Focused => Window?.FocusedWidget == this;
    public bool ShowFocus => Focused && Window!.KeyboardFocus;

    public UiWindow? Window
    {
        get
        {
            Widget w = this;
            while (w.Parent is not null) w = w.Parent;
            return (w as Container)?.Owner;
        }
    }

    public Rectangle AbsBounds =>
        Parent is null ? _bounds : new Rectangle(Parent.AbsBounds.Location + (Size)_bounds.Location, _bounds.Size);

    public void Invalidate() => Window?.Invalidate();

    internal void ResizeQuiet(int width) => _bounds.Width = width;

    public abstract void Paint(Gfx g);

    internal virtual void MouseDown(int x, int y, int clicks) { }
    internal virtual void MouseUp(int x, int y) { }
    internal virtual void MouseMove(int x, int y) { }
    internal virtual void MouseWheel(int dy) { }
    internal virtual bool KeyDown(int sym, int mod) => false;
    internal virtual void TextInput(string text) { }
    internal virtual void FocusChanged() { }
}

internal class Container : Widget
{
    private readonly List<Widget> _children = [];

    public IReadOnlyList<Widget> Children => _children;
    internal UiWindow? Owner { get; set; }

    public T Add<T>(T child) where T : Widget
    {
        child.Parent = this;
        _children.Add(child);
        Invalidate();
        return child;
    }

    public override void Paint(Gfx g)
    {
        foreach (var child in _children)
        {
            if (!child.Visible) continue;
            child.Paint(g);
        }
    }

    internal Widget? HitTest(int x, int y)
    {
        for (var i = _children.Count - 1; i >= 0; i--)
        {
            var child = _children[i];
            if (!child.Visible || !child.AbsBounds.Contains(x, y)) continue;
            if (child is Container inner && inner.HitTest(x, y) is { } deeper) return deeper;
            return child;
        }
        return null;
    }

    internal void CollectTabStops(List<Widget> into)
    {
        foreach (var child in _children)
        {
            if (!child.Visible) continue;
            if (child.TabStop && child.Enabled) into.Add(child);
            if (child is Container inner) inner.CollectTabStops(into);
        }
    }
}
