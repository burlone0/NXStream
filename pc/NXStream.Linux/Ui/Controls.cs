using System.Drawing;

namespace NXStream.Linux.Ui;

internal sealed class Label : Widget
{
    private string _text = "";

    public string Text
    {
        get => _text;
        set
        {
            if (_text == value) return;
            _text = value;
            Invalidate();
        }
    }

    public UiFont Font { get; set; } = UiFont.Body;
    public bool Muted { get; set; }
    public bool Wrap { get; set; }

    public override void Paint(Gfx g)
    {
        var p = g.Palette;
        var color = !Enabled ? p.Disabled : Muted ? p.Muted : p.Text;
        var r = g.Px(AbsBounds);
        if (Wrap)
        {
            var old = g.Canvas.SetClip(r);
            var lineHeight = g.TextHeight(Font);
            var maxLines = Math.Max(1, r.Height / lineHeight);
            var lines = TextRenderer.Wrap(_text, Font, g.Scale, r.Width);
            var y = r.Y;
            for (var i = 0; i < lines.Count && i < maxLines; i++)
            {
                var line = lines[i];
                if (i == maxLines - 1 && lines.Count > maxLines)
                    line = TextRenderer.Fit(string.Join(" ", lines.Skip(i)), Font, g.Scale, r.Width);
                g.Text(line, Font, r.X, y, color);
                y += lineHeight;
            }
            g.Canvas.SetClip(old);
            return;
        }
        var text = Bounds.Width > 0 ? TextRenderer.Fit(_text, Font, g.Scale, r.Width) : _text;
        g.Text(text, Font, r.X, r.Y, color);
    }
}

internal sealed class Button : Widget
{
    public string Text { get; set; } = "";
    public event Action? Click;

    public override bool Interactive => true;
    public override bool TabStop => true;

    private bool IsDefault => Window is { } w && w.AcceptButton == this && (w.FocusedWidget is not Button || w.FocusedWidget == this);

    public void PerformClick()
    {
        if (Enabled) Click?.Invoke();
    }

    internal override void MouseUp(int x, int y)
    {
        if (AbsBounds.Contains(x, y)) PerformClick();
    }

    internal override bool KeyDown(int sym, int mod)
    {
        if (sym is not (Keys.Space or Keys.Return or Keys.KpEnter)) return false;
        PerformClick();
        return true;
    }

    public override void Paint(Gfx g)
    {
        var p = g.Palette;
        var r = g.Px(AbsBounds);
        var rf = new RectangleF(r.X, r.Y, r.Width, r.Height);
        var radius = g.S(4);
        Color text;
        if (!Enabled)
        {
            g.Canvas.FillRoundRect(rf, radius, p.ButtonPressed);
            g.Canvas.StrokeRoundRect(rf, radius, 1, p.Border);
            text = p.Disabled;
        }
        else if (IsDefault)
        {
            g.Canvas.FillRoundRect(rf, radius, Pressed ? Color.FromArgb(200, p.Accent) : Hover ? p.AccentHover : p.Accent);
            text = p.OnAccent;
        }
        else
        {
            g.Canvas.FillRoundRect(rf, radius, Pressed ? p.ButtonPressed : Hover ? p.ButtonHover : p.ButtonFill);
            g.Canvas.StrokeRoundRect(rf, radius, 1, p.Border);
            text = p.Text;
        }
        g.TextCentered(Text, UiFont.Body, r, text);
        if (ShowFocus)
        {
            var inner = new RectangleF(r.X + g.S(2), r.Y + g.S(2), r.Width - g.S(4), r.Height - g.S(4));
            g.Canvas.StrokeRoundRect(inner, radius - 1, 1, IsDefault ? p.OnAccent : p.Text);
        }
    }
}

internal abstract class ToggleButton : Widget
{
    private string _text = "";
    private bool _checked;

    public string Text
    {
        get => _text;
        set
        {
            _text = value;
            Invalidate();
        }
    }

    public bool Checked
    {
        get => _checked;
        set
        {
            if (_checked == value) return;
            SetChecked(value);
        }
    }

    public event Action? CheckedChanged;

    public override bool Interactive => true;
    public override bool TabStop => true;

    protected virtual void SetChecked(bool value)
    {
        _checked = value;
        Invalidate();
        CheckedChanged?.Invoke();
    }

    protected abstract void Activate();
    protected abstract void DrawGlyph(Gfx g, Rectangle box);

    internal override void MouseUp(int x, int y)
    {
        if (AbsBounds.Contains(x, y)) Activate();
    }

    internal override bool KeyDown(int sym, int mod)
    {
        if (sym != Keys.Space) return false;
        Activate();
        return true;
    }

    public override void Paint(Gfx g)
    {
        var r = g.Px(AbsBounds);
        var size = g.S(16);
        var gap = g.S(8);
        var textWidth = g.TextWidth(_text, UiFont.Body);
        ResizeQuiet((int)Math.Ceiling((size + gap + textWidth) / g.Scale) + 2);
        DrawGlyph(g, new Rectangle(r.X, r.Y + (r.Height - size) / 2, size, size));
        var textHeight = g.TextHeight(UiFont.Body);
        var textY = r.Y + (r.Height - textHeight) / 2;
        g.Text(_text, UiFont.Body, r.X + size + gap, textY, Enabled ? g.Palette.Text : g.Palette.Disabled);
        if (ShowFocus)
            g.DottedRect(new Rectangle(r.X + size + gap - g.S(2), textY - g.S(1), textWidth + g.S(4), textHeight + g.S(2)), g.Palette.Text);
    }
}

internal sealed class CheckBox : ToggleButton
{
    protected override void Activate() => Checked = !Checked;

    protected override void DrawGlyph(Gfx g, Rectangle box)
    {
        var p = g.Palette;
        var rf = new RectangleF(box.X, box.Y, box.Width, box.Height);
        var radius = g.S(4);
        if (Checked)
        {
            var fill = !Enabled ? p.Disabled : Pressed ? Color.FromArgb(200, p.Accent) : Hover ? p.AccentHover : p.Accent;
            g.Canvas.FillRoundRect(rf, radius, fill);
            var s = box.Width;
            var t = Math.Max(1.5f, s * 0.11f);
            g.Canvas.Line(box.X + s * 0.25f, box.Y + s * 0.53f, box.X + s * 0.42f, box.Y + s * 0.70f, t, p.OnAccent);
            g.Canvas.Line(box.X + s * 0.42f, box.Y + s * 0.70f, box.X + s * 0.76f, box.Y + s * 0.32f, t, p.OnAccent);
            return;
        }
        g.Canvas.FillRoundRect(rf, radius, Pressed ? p.ButtonPressed : Hover ? p.ButtonHover : p.Surface);
        g.Canvas.StrokeRoundRect(rf, radius, 1, Enabled ? p.ControlBorder : p.Disabled);
    }
}

internal sealed class RadioButton : ToggleButton
{
    private IEnumerable<RadioButton> Group => Parent?.Children.OfType<RadioButton>() ?? [];

    public override bool TabStop => Enabled && (Checked || (!Group.Any(r => r.Checked) && Group.First() == this));
    public override bool Focusable => true;

    protected override void SetChecked(bool value)
    {
        if (value)
            foreach (var other in Group.Where(r => r != this && r.Checked).ToList())
                other.Checked = false;
        base.SetChecked(value);
    }

    protected override void Activate() => Checked = true;

    internal override bool KeyDown(int sym, int mod)
    {
        var step = sym is Keys.Left or Keys.Up ? -1 : sym is Keys.Right or Keys.Down ? 1 : 0;
        if (step == 0) return base.KeyDown(sym, mod);
        var group = Group.Where(r => r.Enabled && r.Visible).ToList();
        var next = group[(group.IndexOf(this) + step + group.Count) % group.Count];
        next.Checked = true;
        Window?.Focus(next);
        return true;
    }

    protected override void DrawGlyph(Gfx g, Rectangle box)
    {
        var p = g.Palette;
        var rf = new RectangleF(box.X, box.Y, box.Width, box.Height);
        if (Checked)
        {
            var fill = !Enabled ? p.Disabled : Pressed ? Color.FromArgb(200, p.Accent) : Hover ? p.AccentHover : p.Accent;
            g.Canvas.FillCircle(rf, fill);
            var dot = box.Width * (Pressed ? 0.28f : Hover ? 0.46f : 0.40f);
            g.Canvas.FillCircle(new RectangleF(box.X + (box.Width - dot) / 2, box.Y + (box.Height - dot) / 2, dot, dot), p.OnAccent);
            return;
        }
        g.Canvas.FillCircle(rf, Pressed ? p.ButtonPressed : Hover ? p.ButtonHover : p.Surface);
        g.Canvas.StrokeCircle(rf, 1, Enabled ? p.ControlBorder : p.Disabled);
    }
}

internal sealed class LinkLabel : Widget
{
    public string Text { get; set; } = "";
    public event Action? Click;

    public override bool Interactive => true;
    public override bool TabStop => true;
    public override UiCursor Cursor => UiCursor.Hand;

    internal override void MouseUp(int x, int y)
    {
        if (AbsBounds.Contains(x, y)) Click?.Invoke();
    }

    internal override bool KeyDown(int sym, int mod)
    {
        if (sym is not (Keys.Space or Keys.Return or Keys.KpEnter)) return false;
        Click?.Invoke();
        return true;
    }

    public override void Paint(Gfx g)
    {
        var r = g.Px(AbsBounds);
        var width = g.TextWidth(Text, UiFont.Body);
        ResizeQuiet((int)Math.Ceiling(width / g.Scale) + 2);
        var height = g.TextHeight(UiFont.Body);
        var y = r.Y + (r.Height - height) / 2;
        var color = !Enabled ? g.Palette.Disabled : Hover || Pressed ? g.Palette.Text : g.Palette.Link;
        g.Text(Text, UiFont.Body, r.X, y, color);
        g.Canvas.FillRect(new Rectangle(r.X, y + height - g.S(2), width, Math.Max(1, g.S(1))), color);
        if (ShowFocus) g.DottedRect(new Rectangle(r.X - g.S(2), y - g.S(1), width + g.S(4), height + g.S(2)), g.Palette.Text);
    }
}

internal sealed class GroupBox : Container
{
    public string Text { get; set; } = "";

    public override void Paint(Gfx g)
    {
        var p = g.Palette;
        var r = g.Px(AbsBounds);
        var th = g.TextHeight(UiFont.Body);
        var top = r.Y + th / 2;
        g.Canvas.StrokeRoundRect(new RectangleF(r.X, top, r.Width, r.Bottom - top), g.S(4), 1, p.Border);
        var textWidth = g.TextWidth(Text, UiFont.Body);
        g.Canvas.FillRect(new Rectangle(r.X + g.S(8), r.Y, textWidth + g.S(8), th), p.Back);
        g.Text(Text, UiFont.Body, r.X + g.S(12), r.Y, Enabled ? p.Text : p.Disabled);
        base.Paint(g);
    }
}
