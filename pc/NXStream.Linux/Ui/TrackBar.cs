using System.Drawing;

namespace NXStream.Linux.Ui;

internal sealed class TrackBar : Widget
{
    private const int Margin = 10;
    private int _value;

    public int Minimum { get; set; }
    public int Maximum { get; set; } = 10;
    public int TickFrequency { get; set; } = 1;
    public int SmallChange { get; set; } = 1;
    public int LargeChange { get; set; } = 5;
    public event Action? ValueChanged;

    public override bool Interactive => true;
    public override bool TabStop => true;

    public int Value
    {
        get => _value;
        set
        {
            var clamped = Math.Clamp(value, Minimum, Maximum);
            if (clamped == _value) return;
            _value = clamped;
            Invalidate();
            ValueChanged?.Invoke();
        }
    }

    private void SetFromX(int x)
    {
        var left = AbsBounds.X + Margin;
        var width = Math.Max(1, AbsBounds.Width - 2 * Margin);
        Value = Minimum + (int)Math.Round((double)(x - left) / width * (Maximum - Minimum));
    }

    internal override void MouseDown(int x, int y, int clicks) => SetFromX(x);

    internal override void MouseMove(int x, int y)
    {
        if (Pressed) SetFromX(x);
    }

    internal override bool KeyDown(int sym, int mod)
    {
        switch (sym)
        {
            case Keys.Left or Keys.Down: Value -= SmallChange; break;
            case Keys.Right or Keys.Up: Value += SmallChange; break;
            case Keys.PageDown: Value -= LargeChange; break;
            case Keys.PageUp: Value += LargeChange; break;
            case Keys.Home: Value = Minimum; break;
            case Keys.End: Value = Maximum; break;
            default: return false;
        }
        return true;
    }

    public override void Paint(Gfx g)
    {
        var p = g.Palette;
        var r = g.Px(AbsBounds);
        var margin = g.S(Margin);
        var left = r.X + margin;
        var right = r.Right - margin;
        var railY = r.Y + Math.Min(r.Height / 2, g.S(17));
        var thickness = Math.Max(2, g.S(4));
        var span = Math.Max(1, Maximum - Minimum);
        float XOf(int v) => left + (float)(v - Minimum) / span * (right - left);
        var thumbX = XOf(_value);
        var accent = Enabled ? p.Accent : p.Disabled;

        g.Canvas.FillRoundRect(new RectangleF(left, railY - thickness / 2f, right - left, thickness), thickness / 2f, p.Rail);
        g.Canvas.FillRoundRect(new RectangleF(left, railY - thickness / 2f, thumbX - left, thickness), thickness / 2f, accent);
        if (TickFrequency > 0)
            for (var v = Minimum; v <= Maximum; v += TickFrequency)
            {
                var tick = new Rectangle((int)Math.Round(XOf(v)), railY + g.S(11), Math.Max(1, g.S(1)), g.S(4));
                g.Canvas.FillRect(tick, Enabled ? p.Rail : p.Disabled);
            }

        var outer = g.S(20);
        var inner = g.S(Pressed ? 10 : Hover && Enabled ? 14 : 12);
        var outerRect = new RectangleF(thumbX - outer / 2f, railY - outer / 2f, outer, outer);
        g.Canvas.FillCircle(outerRect, p.Dark ? p.ButtonHover : p.Surface);
        g.Canvas.StrokeCircle(outerRect, 1, p.Border);
        g.Canvas.FillCircle(new RectangleF(thumbX - inner / 2f, railY - inner / 2f, inner, inner), accent);
        if (ShowFocus)
            g.Canvas.StrokeCircle(new RectangleF(outerRect.X - g.S(3), outerRect.Y - g.S(3), outer + g.S(6), outer + g.S(6)), 1, p.Text);
    }
}
