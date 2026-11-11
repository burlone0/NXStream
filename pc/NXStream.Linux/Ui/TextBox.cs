using System.Drawing;

namespace NXStream.Linux.Ui;

internal sealed class TextBox : Widget
{
    private const int PadX = 6;
    private string _text = "";
    private int _caret, _anchor, _scroll;
    private bool _dragging;

    public string PlaceholderText { get; set; } = "";

    public override bool Interactive => true;
    public override bool TabStop => true;
    public override UiCursor Cursor => UiCursor.IBeam;

    public string Text
    {
        get => _text;
        set
        {
            value ??= "";
            if (_text == value) return;
            _text = value;
            _caret = _anchor = value.Length;
            Invalidate();
        }
    }

    public void SelectAll()
    {
        _anchor = 0;
        _caret = _text.Length;
        Invalidate();
    }

    private float Scale => Window?.Scale ?? 1f;

    private int WidthOf(int length) => TextRenderer.Width(_text[..length], UiFont.Body, Scale);

    private int Prev(int i) => i <= 0 ? 0 : char.IsLowSurrogate(_text[i - 1]) && i > 1 ? i - 2 : i - 1;

    private int Next(int i) => i >= _text.Length ? _text.Length : char.IsHighSurrogate(_text[i]) && i + 1 < _text.Length ? i + 2 : i + 1;

    private int IndexAt(int xDesign)
    {
        var px = xDesign * Scale - (AbsBounds.X + PadX) * Scale + _scroll;
        var prev = 0;
        var prevWidth = 0;
        for (var i = Next(0); ; i = Next(i))
        {
            if (_text.Length == 0) return 0;
            var w = WidthOf(i);
            if (w >= px) return px - prevWidth < w - px ? prev : i;
            if (i >= _text.Length) return _text.Length;
            prev = i;
            prevWidth = w;
        }
    }

    private void Replace(string insert)
    {
        var from = Math.Min(_caret, _anchor);
        var to = Math.Max(_caret, _anchor);
        _text = _text[..from] + insert + _text[to..];
        _caret = _anchor = from + insert.Length;
        Invalidate();
    }

    private string Selected => _text[Math.Min(_caret, _anchor)..Math.Max(_caret, _anchor)];

    internal override void TextInput(string text)
    {
        var clean = new string(text.Where(c => !char.IsControl(c)).ToArray());
        if (clean.Length > 0) Replace(clean);
    }

    internal override void MouseDown(int x, int y, int clicks)
    {
        if (clicks >= 2)
        {
            SelectAll();
            _dragging = false;
            return;
        }
        _caret = _anchor = IndexAt(x);
        _dragging = true;
    }

    internal override void MouseMove(int x, int y)
    {
        if (!Pressed || !_dragging) return;
        var index = IndexAt(x);
        if (index == _caret) return;
        _caret = index;
        Invalidate();
    }

    internal override bool KeyDown(int sym, int mod)
    {
        var shift = (mod & Keys.ModShift) != 0;
        if ((mod & Keys.ModCtrl) != 0)
        {
            switch (sym)
            {
                case 'a': SelectAll(); return true;
                case 'c':
                    if (_caret != _anchor) Sdl.SDL_SetClipboardText(Selected);
                    return true;
                case 'x':
                    if (_caret != _anchor)
                    {
                        Sdl.SDL_SetClipboardText(Selected);
                        Replace("");
                    }
                    return true;
                case 'v':
                    TextInput(Sdl.Clipboard());
                    return true;
            }
            return false;
        }
        switch (sym)
        {
            case Keys.Left:
                Move(_caret != _anchor && !shift ? Math.Min(_caret, _anchor) : Prev(_caret), shift);
                return true;
            case Keys.Right:
                Move(_caret != _anchor && !shift ? Math.Max(_caret, _anchor) : Next(_caret), shift);
                return true;
            case Keys.Home: Move(0, shift); return true;
            case Keys.End: Move(_text.Length, shift); return true;
            case Keys.Backspace:
                if (_caret == _anchor) _anchor = Prev(_caret);
                Replace("");
                return true;
            case Keys.Delete:
                if (_caret == _anchor) _anchor = Next(_caret);
                Replace("");
                return true;
        }
        return false;
    }

    private void Move(int to, bool extend)
    {
        _caret = to;
        if (!extend) _anchor = to;
        Invalidate();
    }

    public override void Paint(Gfx g)
    {
        var p = g.Palette;
        var r = g.Px(AbsBounds);
        var rf = new RectangleF(r.X, r.Y, r.Width, r.Height);
        var radius = g.S(4);
        g.Canvas.FillRoundRect(rf, radius, Enabled ? p.Surface : p.ButtonPressed);
        g.Canvas.StrokeRoundRect(rf, radius, 1, Hover && Enabled ? p.ControlBorder : p.Border);
        if (Focused)
        {
            var line = Math.Max(2, g.S(2));
            var old = g.Canvas.SetClip(new Rectangle(r.X, r.Bottom - line, r.Width, line));
            g.Canvas.StrokeRoundRect(new RectangleF(r.X, r.Y, r.Width, r.Height), radius, line, p.Accent);
            g.Canvas.SetClip(old);
        }

        var pad = g.S(PadX);
        var inner = new Rectangle(r.X + pad, r.Y, Math.Max(1, r.Width - 2 * pad), r.Height);
        var th = g.TextHeight(UiFont.Body);
        var y = r.Y + (r.Height - th) / 2;
        var oldClip = g.Canvas.SetClip(inner);
        var caretX = WidthOf(_caret);
        if (caretX - _scroll > inner.Width - 2) _scroll = caretX - inner.Width + 2;
        if (caretX < _scroll) _scroll = caretX;
        if (g.TextWidth(_text, UiFont.Body) <= inner.Width) _scroll = 0;
        if (_text.Length == 0)
        {
            g.Text(PlaceholderText, UiFont.Body, inner.X, y, p.Muted);
        }
        else
        {
            if (_caret != _anchor)
            {
                var a = inner.X - _scroll + WidthOf(Math.Min(_caret, _anchor));
                var b = inner.X - _scroll + WidthOf(Math.Max(_caret, _anchor));
                g.Canvas.FillRect(new Rectangle(a, y - g.S(1), b - a, th + g.S(2)), p.Selection);
            }
            g.Text(_text, UiFont.Body, inner.X - _scroll, y, Enabled ? p.Text : p.Disabled);
        }
        if (Focused && Enabled && (Window?.CaretOn ?? true) && _caret == _anchor)
            g.Canvas.FillRect(new Rectangle(inner.X - _scroll + caretX, y, Math.Max(1, g.S(1)), th), p.Text);
        g.Canvas.SetClip(oldClip);
    }
}
