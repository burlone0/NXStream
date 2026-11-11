using System.Drawing;
using NXStream.Render;

namespace NXStream.Linux.Ui;

internal sealed class MenuItem
{
    public string Text { get; set; } = "";
    public string? Shortcut { get; set; }
    public bool Separator { get; init; }
    public Action? Click { get; set; }
}

internal sealed class MenuModel
{
    private const int ItemHeight = 24, SeparatorHeight = 9, PadY = 4, PadX = 12, Gap = 28;
    private readonly List<MenuItem> _items = [];
    private readonly List<Rectangle> _rects = [];

    public int HoverIndex { get; set; } = -1;

    public MenuItem Add(string text, string? shortcut = null, Action? click = null)
    {
        var item = new MenuItem { Text = text, Shortcut = shortcut, Click = click };
        _items.Add(item);
        return item;
    }

    public void AddSeparator() => _items.Add(new MenuItem { Separator = true });

    public int HitTest(int x, int y)
    {
        for (var i = 0; i < _rects.Count; i++)
            if (!_items[i].Separator && _rects[i].Contains(x, y)) return i;
        return -1;
    }

    public bool Hover(int x, int y)
    {
        var index = HitTest(x, y);
        if (index == HoverIndex) return false;
        HoverIndex = index;
        return true;
    }

    public void Move(int step)
    {
        var count = _items.Count;
        var index = HoverIndex < 0 && step < 0 ? count : HoverIndex;
        do index = (index + step + count) % count;
        while (_items[index].Separator);
        HoverIndex = index;
    }

    public bool Activate(int index)
    {
        if (index < 0 || index >= _items.Count || _items[index].Separator) return false;
        _items[index].Click?.Invoke();
        return true;
    }

    public OverlayImage RenderToBgra(float scale, UiPalette p)
    {
        int S(float v) => (int)Math.Round(v * scale);
        var font = UiFont.Body;
        var left = S(PadX);
        var textWidth = _items.Where(i => !i.Separator).Select(i => TextRenderer.Width(i.Text, font, scale)).DefaultIfEmpty(0).Max();
        var shortcutWidth = _items.Where(i => i.Shortcut is not null)
            .Select(i => TextRenderer.Width(i.Shortcut!, font, scale)).DefaultIfEmpty(0).Max();
        var width = left + textWidth + (shortcutWidth > 0 ? S(Gap) + shortcutWidth : 0) + S(PadX);
        var height = S(PadY) * 2 + _items.Sum(i => i.Separator ? S(SeparatorHeight) : S(ItemHeight));
        var canvas = new Canvas(width, height);
        canvas.Clear(p.Surface);
        _rects.Clear();
        var y = S(PadY);
        var th = TextRenderer.Height(font, scale);
        for (var i = 0; i < _items.Count; i++)
        {
            var item = _items[i];
            var h = item.Separator ? S(SeparatorHeight) : S(ItemHeight);
            var rect = new Rectangle(0, y, width, h);
            _rects.Add(rect);
            if (item.Separator)
            {
                canvas.FillRect(new Rectangle(S(PadX - 4), y + h / 2, width - 2 * S(PadX - 4), 1), p.Border);
            }
            else
            {
                if (i == HoverIndex)
                    canvas.FillRoundRect(new RectangleF(S(4), y + S(1), width - 2 * S(4), h - S(2)), S(4), p.Hover);
                var ty = y + (h - th) / 2;
                TextRenderer.Draw(canvas, item.Text, font, scale, left, ty, p.Text);
                if (item.Shortcut is not null)
                {
                    var shortcutX = width - S(PadX) - TextRenderer.Width(item.Shortcut, font, scale);
                    TextRenderer.Draw(canvas, item.Shortcut, font, scale, shortcutX, ty, p.Muted);
                }
            }
            y += h;
        }
        canvas.StrokeRect(new Rectangle(0, 0, width, height), p.Dark ? p.Border : p.ControlBorder);
        return new OverlayImage(canvas.ToBgra(), width, height, false, 1f);
    }
}
