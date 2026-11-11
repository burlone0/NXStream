using System.Drawing;

namespace NXStream.Linux.Ui;

internal sealed class ListItem
{
    public ListItem(params string[] cells) => Cells = cells;

    public string[] Cells { get; }
    public object? Tag { get; set; }
}

internal sealed class ListColumn
{
    public ListColumn(string text, int width)
    {
        Text = text;
        Width = width;
    }

    public string Text { get; }
    public int Width { get; set; }
}

internal sealed class ListView : Widget
{
    private const int HeaderHeight = 24, RowHeight = 22, CellPad = 8;
    private readonly List<ListColumn> _columns = [];
    private readonly List<ListItem> _items = [];
    private int _selected = -1, _hoverRow = -1, _scroll;

    public IReadOnlyList<ListItem> Items => _items;
    public int Count => _items.Count;
    public event Action? SelectedIndexChanged;
    public event Action? ItemActivated;

    public override bool Interactive => true;
    public override bool TabStop => true;

    public int ClientWidth => Bounds.Width - 2;

    public ListItem? SelectedItem => _selected >= 0 && _selected < _items.Count ? _items[_selected] : null;

    public int SelectedIndex
    {
        get => _selected;
        set
        {
            value = Math.Clamp(value, -1, _items.Count - 1);
            if (value == _selected) return;
            _selected = value;
            EnsureVisible();
            Invalidate();
            SelectedIndexChanged?.Invoke();
        }
    }

    public void AddColumn(string text, int width)
    {
        _columns.Add(new ListColumn(text, width));
        Invalidate();
    }

    public void FillLastColumn()
    {
        if (_columns.Count == 0) return;
        var used = _columns.Take(_columns.Count - 1).Sum(c => c.Width);
        _columns[^1].Width = Math.Max(60, ClientWidth - used);
        Invalidate();
    }

    public void Add(ListItem item)
    {
        _items.Add(item);
        Invalidate();
    }

    public void Clear()
    {
        _items.Clear();
        var changed = _selected != -1;
        _selected = -1;
        _scroll = 0;
        Invalidate();
        if (changed) SelectedIndexChanged?.Invoke();
    }

    private int ViewHeight => Bounds.Height - HeaderHeight - 2;

    private void ClampScroll() => _scroll = Math.Clamp(_scroll, 0, Math.Max(0, _items.Count * RowHeight - ViewHeight));

    private void EnsureVisible()
    {
        if (_selected < 0) return;
        if (_selected * RowHeight < _scroll) _scroll = _selected * RowHeight;
        else if ((_selected + 1) * RowHeight > _scroll + ViewHeight) _scroll = (_selected + 1) * RowHeight - ViewHeight;
        ClampScroll();
    }

    private int RowAt(int y)
    {
        var rel = y - AbsBounds.Y - 1 - HeaderHeight + _scroll;
        if (y - AbsBounds.Y - 1 < HeaderHeight || rel < 0) return -1;
        var row = rel / RowHeight;
        return row < _items.Count ? row : -1;
    }

    internal override void MouseDown(int x, int y, int clicks)
    {
        var row = RowAt(y);
        if (row < 0) return;
        SelectedIndex = row;
        if (clicks >= 2) ItemActivated?.Invoke();
    }

    internal override void MouseMove(int x, int y)
    {
        var row = RowAt(y);
        if (row == _hoverRow) return;
        _hoverRow = row;
        Invalidate();
    }

    internal override void MouseWheel(int dy)
    {
        _scroll -= dy * RowHeight * 3;
        ClampScroll();
        Invalidate();
    }

    internal override bool KeyDown(int sym, int mod)
    {
        var page = Math.Max(1, ViewHeight / RowHeight - 1);
        switch (sym)
        {
            case Keys.Down: SelectedIndex = _selected < 0 ? 0 : _selected + 1; return true;
            case Keys.Up: SelectedIndex = _selected < 0 ? 0 : _selected - 1; return true;
            case Keys.PageDown: SelectedIndex = _selected + page; return true;
            case Keys.PageUp: SelectedIndex = _selected - page; return true;
            case Keys.Home: SelectedIndex = 0; return true;
            case Keys.End: SelectedIndex = _items.Count - 1; return true;
            case Keys.Return or Keys.KpEnter:
                if (_selected < 0) return false;
                ItemActivated?.Invoke();
                return true;
        }
        return false;
    }

    public override void Paint(Gfx g)
    {
        var p = g.Palette;
        var r = g.Px(AbsBounds);
        g.Canvas.FillRect(r, p.Surface);
        var head = g.S(HeaderHeight);
        var row = g.S(RowHeight);
        var body = new Rectangle(r.X + 1, r.Y + 1 + head, r.Width - 2, r.Height - 2 - head);
        var text = Enabled ? p.Text : p.Disabled;
        var th = g.TextHeight(UiFont.Body);
        var scroll = (int)Math.Round(_scroll * g.Scale);

        var old = g.Canvas.SetClip(body);
        for (var i = Math.Max(0, scroll / row); i < _items.Count; i++)
        {
            var y = body.Y + i * row - scroll;
            if (y >= body.Bottom) break;
            var selected = i == _selected;
            if (selected || (Hover && Enabled && i == _hoverRow))
            {
                var highlight = new RectangleF(body.X + g.S(3), y + g.S(1), body.Width - g.S(6), row - g.S(2));
                g.Canvas.FillRoundRect(highlight, g.S(3), selected ? p.Selection : p.Hover);
            }
            var x = r.X + 1;
            for (var c = 0; c < _columns.Count && c < _items[i].Cells.Length; c++)
            {
                var width = g.S(_columns[c].Width);
                var cell = TextRenderer.Fit(_items[i].Cells[c], UiFont.Body, g.Scale, width - 2 * g.S(CellPad));
                g.Text(cell, UiFont.Body, x + g.S(CellPad), y + (row - th) / 2, selected && Enabled ? p.SelectionText : text);
                x += width;
            }
        }
        var total = _items.Count * row;
        if (total > body.Height)
        {
            var thumb = Math.Max(g.S(24), body.Height * body.Height / total);
            var top = body.Y + (int)((long)(body.Height - thumb) * scroll / Math.Max(1, total - body.Height));
            var bar = new RectangleF(body.Right - g.S(7), top + g.S(2), g.S(4), thumb - g.S(4));
            g.Canvas.FillRoundRect(bar, g.S(2), Color.FromArgb(150, p.Rail));
        }
        g.Canvas.SetClip(old);

        var headRect = new Rectangle(r.X + 1, r.Y + 1, r.Width - 2, head);
        g.Canvas.FillRect(headRect, p.HeaderBack);
        g.Canvas.FillRect(new Rectangle(headRect.X, headRect.Bottom - 1, headRect.Width, 1), p.Border);
        var hx = headRect.X;
        var oldHead = g.Canvas.SetClip(headRect);
        foreach (var column in _columns)
        {
            var width = g.S(column.Width);
            var label = TextRenderer.Fit(column.Text, UiFont.Body, g.Scale, width - 2 * g.S(6));
            g.Text(label, UiFont.Body, hx + g.S(6), headRect.Y + (head - th) / 2, text);
            hx += width;
            g.Canvas.FillRect(new Rectangle(hx - 1, headRect.Y + g.S(4), 1, head - g.S(8)), p.Border);
        }
        g.Canvas.SetClip(oldHead);
        g.Canvas.StrokeRect(r, p.Border);
    }
}
